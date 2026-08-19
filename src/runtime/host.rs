use super::assets;
use crate::model::context::RenderContext;
use luna::{Callback, CallbackReturn, Context, Error, FromValue, Table, Value};
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::fs::{File, OpenOptions};
use std::io::Read;
use std::os::unix::fs::{DirBuilderExt, MetadataExt, OpenOptionsExt, PermissionsExt};
use std::os::unix::process::CommandExt;
use std::path::{Component, Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use unicode_width::UnicodeWidthStr;
use wait_timeout::ChildExt;

const MAX_READ: usize = 64 * 1024;
const MAX_EXEC_OUTPUT: usize = 64 * 1024;
const MAX_EXEC_TIMEOUT_MS: u64 = 2_000;
const MAX_CACHE_ENTRIES: usize = 256;
const MAX_CACHE_TTL_MS: u64 = 60 * 60 * 1_000;
const MAX_EXEC_ARGS: usize = 128;
const MAX_EXEC_INPUT: usize = 64 * 1024;
const MAX_CACHE_FILE: usize = 4 * 1024 * 1024;
/// Total time one render may spend blocked in host I/O, on top of its Lua deadline.
const MAX_RENDER_IO: Duration = Duration::from_millis(2_000);
const MAX_ASSET_ENTRIES: usize = 64;
const MAX_ASSET_BYTES: usize = 8 * 1024 * 1024;

#[derive(Clone, Default)]
pub struct HostState {
    inner: Arc<HostInner>,
}

#[derive(Default)]
struct HostInner {
    context: Mutex<RenderContext>,
    roots: Mutex<Vec<PathBuf>>,
    data_dir: Mutex<Option<PathBuf>>,
    cache_dir: Mutex<Option<PathBuf>>,
    cache: Mutex<HashMap<String, CacheEntry>>,
    io_spent: Mutex<Duration>,
    assets: Mutex<AssetCache>,
}

/// Decoded assets, memoised for the process and bounded by entry count and
/// total bytes.
#[derive(Default)]
struct AssetCache {
    entries: HashMap<(String, String), Option<Arc<str>>>,
    bytes: usize,
}

#[derive(Clone)]
struct CacheEntry {
    expires: Instant,
    value: ExecResult,
}

#[derive(Clone, Deserialize, Serialize)]
struct ExecResult {
    status: i32,
    stdout: String,
    stderr: String,
    timed_out: bool,
    truncated: bool,
}

#[derive(Deserialize, Serialize)]
struct DiskCacheEntry {
    version: u16,
    key_hash: String,
    created_at_ms: u64,
    expires_at_ms: u64,
    value: ExecResult,
}

impl HostState {
    pub fn set_context(&self, context: RenderContext) {
        *self.inner.context.lock().expect("context lock") = context;
    }

    pub fn set_roots(&self, roots: Vec<PathBuf>) {
        *self.inner.roots.lock().expect("roots lock") = roots;
    }

    pub fn set_data_dir(&self, data_dir: PathBuf) {
        *self.inner.data_dir.lock().expect("data directory lock") = Some(data_dir);
    }

    pub fn set_cache_dir(&self, cache_dir: PathBuf) {
        *self.inner.cache_dir.lock().expect("cache directory lock") = Some(cache_dir);
    }

    /// Reset the per-render I/O budget. Called once before each render.
    pub fn begin_render(&self) {
        *self.inner.io_spent.lock().expect("io lock") = Duration::ZERO;
    }

    /// Time this render has spent blocked in host I/O, bounded by `MAX_RENDER_IO`.
    pub fn io_spent(&self) -> Duration {
        *self.inner.io_spent.lock().expect("io lock")
    }

    fn io_remaining(&self) -> Duration {
        MAX_RENDER_IO.saturating_sub(self.io_spent())
    }

    fn cached_asset(&self, pack: &str, name: &str) -> Option<Option<Arc<str>>> {
        let cache = self.inner.assets.lock().expect("asset lock");
        cache
            .entries
            .get(&(pack.to_string(), name.to_string()))
            .cloned()
    }

    fn store_asset(&self, pack: String, name: String, text: Option<String>) -> Option<Arc<str>> {
        let text = text.map(Arc::<str>::from);
        let size = text.as_deref().map_or(0, str::len);
        let mut cache = self.inner.assets.lock().expect("asset lock");
        if cache.entries.len() >= MAX_ASSET_ENTRIES || cache.bytes + size > MAX_ASSET_BYTES {
            cache.entries.clear();
            cache.bytes = 0;
        }
        cache.bytes += size;
        cache.entries.insert((pack, name), text.clone());
        text
    }

    fn record_io<T>(&self, call: impl FnOnce() -> T) -> T {
        let started = Instant::now();
        let value = call();
        let mut spent = self.inner.io_spent.lock().expect("io lock");
        *spent = spent.saturating_add(started.elapsed());
        value
    }

    pub fn install<'gc>(&self, ctx: Context<'gc>) -> Result<(), Error<'gc>> {
        let table = Table::new(&ctx);
        table.set(ctx, "platform", std::env::consts::OS)?;

        let host = self.clone();
        table.set(
            ctx,
            "env",
            Callback::from_fn(&ctx, move |ctx, _, mut stack| {
                let name: String = stack.consume(ctx)?;
                if !valid_env_name(&name) {
                    stack.replace(ctx, Value::Nil);
                    return Ok(CallbackReturn::Return);
                }
                let context = host.inner.context.lock().expect("context lock");
                let value = match context.env.get(&name) {
                    Some(value) => value.clone(),
                    None => std::env::var(name).ok(),
                };
                stack.replace(ctx, value);
                Ok(CallbackReturn::Return)
            }),
        )?;

        table.set(
            ctx,
            "cell_width",
            Callback::from_fn(&ctx, |ctx, _, mut stack| {
                let text: String = stack.consume(ctx)?;
                stack.replace(ctx, UnicodeWidthStr::width(text.as_str()));
                Ok(CallbackReturn::Return)
            }),
        )?;

        let host = self.clone();
        table.set(
            ctx,
            "read",
            Callback::from_fn(&ctx, move |ctx, _, mut stack| {
                let path: String = stack.consume(ctx)?;
                let source = host
                    .record_io(|| host.read(&path))
                    .map_err(|error| raise(ctx, error))?;
                stack.replace(ctx, source);
                Ok(CallbackReturn::Return)
            }),
        )?;

        let host = self.clone();
        table.set(
            ctx,
            "exec",
            Callback::from_fn(&ctx, move |ctx, _, mut stack| {
                let (argv, options): (Table, Option<Table>) = stack.consume(ctx)?;
                let mut args: Vec<String> = Vec::new();
                let mut argv_bytes = 0_usize;
                for index in 1_i64.. {
                    let Some(value) = argv.get::<_, Option<String>>(ctx, index)? else {
                        break;
                    };
                    argv_bytes += value.len();
                    if args.len() >= MAX_EXEC_ARGS || argv_bytes > MAX_EXEC_INPUT {
                        return Err(raise(ctx, "exec argv exceeds host limits"));
                    }
                    args.push(value);
                }
                let options = options.unwrap_or_else(|| Table::new(&ctx));
                let timeout_ms = options
                    .get::<_, Option<u64>>(ctx, "timeout_ms")?
                    .unwrap_or(100)
                    .min(MAX_EXEC_TIMEOUT_MS)
                    .min(host.io_remaining().as_millis() as u64);
                if timeout_ms == 0 {
                    return Err(raise(ctx, "render exhausted its host I/O budget"));
                }
                let ttl_ms = options
                    .get::<_, Option<u64>>(ctx, "ttl_ms")?
                    .unwrap_or(0)
                    .min(MAX_CACHE_TTL_MS);
                let cwd = options
                    .get::<_, Option<String>>(ctx, "cwd")?
                    .map(PathBuf::from);
                let mut environment = host
                    .inner
                    .context
                    .lock()
                    .expect("context lock")
                    .env
                    .clone()
                    .into_iter()
                    .collect::<Vec<_>>();
                if let Some(values) = options.get::<_, Option<Table>>(ctx, "env")? {
                    for (name, value) in values.iter(ctx) {
                        let name = String::from_value(ctx, name)?;
                        let value = String::from_value(ctx, value)?;
                        if !valid_env_name(&name) {
                            return Err(raise(ctx, format!("invalid environment name {name:?}")));
                        }
                        if let Some(existing) = environment
                            .iter_mut()
                            .find(|(existing, _)| existing == &name)
                        {
                            existing.1 = Some(value);
                        } else {
                            environment.push((name, Some(value)));
                        }
                    }
                    environment.sort();
                }
                if cwd
                    .as_ref()
                    .is_some_and(|path| path.as_os_str().len() > 4096)
                    || environment.len() > MAX_EXEC_ARGS
                    || environment
                        .iter()
                        .map(|(name, value)| {
                            name.len() + value.as_deref().unwrap_or_default().len()
                        })
                        .sum::<usize>()
                        > MAX_EXEC_INPUT
                {
                    return Err(raise(ctx, "exec options exceed host limits"));
                }
                let result = host
                    .record_io(|| {
                        host.exec(&args, cwd.as_deref(), timeout_ms, ttl_ms, &environment)
                    })
                    .map_err(|error| raise(ctx, error))?;
                let value = Table::new(&ctx);
                value.set(ctx, "status", result.status)?;
                value.set(ctx, "stdout", result.stdout)?;
                value.set(ctx, "stderr", result.stderr)?;
                value.set(ctx, "timed_out", result.timed_out)?;
                value.set(ctx, "truncated", result.truncated)?;
                stack.replace(ctx, value);
                Ok(CallbackReturn::Return)
            }),
        )?;

        let host = self.clone();
        table.set(
            ctx,
            "asset",
            Callback::from_fn(&ctx, move |ctx, _, mut stack| {
                let (pack, name): (String, String) = stack.consume(ctx)?;
                if !crate::model::output::valid_selector(&pack) || name.is_empty() {
                    return Err(raise(ctx, "invalid asset pack or name"));
                }
                if let Some(hit) = host.cached_asset(&pack, &name) {
                    stack.replace(ctx, hit.as_deref());
                    return Ok(CallbackReturn::Return);
                }
                let directory = host
                    .inner
                    .data_dir
                    .lock()
                    .expect("data directory lock")
                    .clone();
                let mut bytes = None;
                if let Some(path) = directory.map(|path| path.join(format!("{pack}.pixypack")))
                    && path.exists()
                {
                    bytes = host
                        .record_io(|| assets::item(&path, &name))
                        .map_err(|error| raise(ctx, error))?;
                }
                if bytes.is_none() {
                    bytes =
                        assets::embedded_item(&pack, &name).map_err(|error| raise(ctx, error))?;
                }
                let text = bytes
                    .map(|bytes| String::from_utf8(bytes).map_err(|error| raise(ctx, error)))
                    .transpose()?;
                let text = host.store_asset(pack, name, text);
                stack.replace(ctx, text.as_deref());
                Ok(CallbackReturn::Return)
            }),
        )?;

        ctx.set_global("__pixy_host", table);
        Ok(())
    }

    fn read(&self, requested: &str) -> Result<Option<String>, String> {
        let path = Path::new(requested);
        if path
            .components()
            .any(|part| matches!(part, Component::ParentDir))
        {
            return Err("parent path components are not allowed".into());
        }
        let candidate = if path.is_absolute() {
            path.to_path_buf()
        } else {
            let roots = self.inner.roots.lock().expect("roots lock");
            let Some(root) = roots.first() else {
                return Err("no trusted file root is configured".into());
            };
            root.join(path)
        };
        let canonical = match candidate.canonicalize() {
            Ok(path) => path,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(None),
            Err(error) => return Err(error.to_string()),
        };
        let roots = self.inner.roots.lock().expect("roots lock");
        if !roots
            .iter()
            .filter_map(|root| root.canonicalize().ok())
            .any(|root| canonical.starts_with(root))
        {
            return Err("file is outside trusted Pixy directories".into());
        }
        let file = File::open(canonical).map_err(|error| error.to_string())?;
        let mut bytes = Vec::new();
        file.take((MAX_READ + 1) as u64)
            .read_to_end(&mut bytes)
            .map_err(|error| error.to_string())?;
        if bytes.len() > MAX_READ {
            return Err(format!("file exceeds {MAX_READ} byte limit"));
        }
        String::from_utf8(bytes)
            .map(Some)
            .map_err(|_| "file is not UTF-8".into())
    }

    fn exec(
        &self,
        argv: &[String],
        cwd: Option<&Path>,
        timeout_ms: u64,
        ttl_ms: u64,
        environment: &[(String, Option<String>)],
    ) -> Result<ExecResult, String> {
        let Some(program) = argv.first() else {
            return Err("exec requires a non-empty argv".into());
        };
        if program.is_empty() {
            return Err("exec program cannot be empty".into());
        }
        let effective_environment = apply_environment(std::env::vars_os().collect(), environment);
        let key = format!(
            "{argv:?}\u{0}{cwd:?}\u{0}{effective_environment:?}\u{0}{timeout_ms}\u{0}{ttl_ms}"
        );
        let cached = self.cached(&key, ttl_ms);
        if let Some(value) = cached {
            return Ok(value);
        }

        let mut command = Command::new(program);
        command
            .args(&argv[1..])
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .process_group(0);
        command.env_clear().envs(effective_environment);
        if let Some(cwd) = cwd {
            command.current_dir(cwd);
        }
        let mut child = command.spawn().map_err(|error| error.to_string())?;
        let stdout = child
            .stdout
            .take()
            .ok_or_else(|| "stdout pipe unavailable".to_string())?;
        let stderr = child
            .stderr
            .take()
            .ok_or_else(|| "stderr pipe unavailable".to_string())?;
        let stdout_reader = thread::spawn(move || read_capped(stdout));
        let stderr_reader = thread::spawn(move || read_capped(stderr));
        let timeout = Duration::from_millis(timeout_ms.min(MAX_EXEC_TIMEOUT_MS));
        let timed_out = child
            .wait_timeout(timeout)
            .map_err(|error| error.to_string())?
            .is_none();
        unsafe { libc::kill(-(child.id() as i32), libc::SIGKILL) };
        if timed_out {
            let _ = child.kill();
        }
        let status = child.wait().map_err(|error| error.to_string())?;
        let (stdout, stdout_truncated) = stdout_reader
            .join()
            .map_err(|_| "stdout reader failed".to_string())??;
        let (stderr, stderr_truncated) = stderr_reader
            .join()
            .map_err(|_| "stderr reader failed".to_string())??;
        let result = ExecResult {
            status: status.code().unwrap_or(if timed_out { 124 } else { 1 }),
            stdout,
            stderr,
            timed_out,
            truncated: stdout_truncated || stderr_truncated,
        };
        self.store_cache(key, ttl_ms, result.clone());
        Ok(result)
    }

    fn cached(&self, key: &str, ttl_ms: u64) -> Option<ExecResult> {
        if ttl_ms == 0 {
            return None;
        }
        let now = Instant::now();
        {
            let mut cache = self.inner.cache.lock().expect("cache lock");
            cache.retain(|_, entry| entry.expires > now);
            if let Some(entry) = cache.get(key) {
                return Some(entry.value.clone());
            }
        }
        let (value, remaining) = self.read_disk_cache(key)?;
        self.store_memory(key.to_string(), remaining, value.clone());
        Some(value)
    }

    fn store_cache(&self, key: String, ttl_ms: u64, value: ExecResult) {
        if ttl_ms == 0 {
            return;
        }
        self.store_memory(key.clone(), Duration::from_millis(ttl_ms), value.clone());
        self.write_disk_cache(&key, ttl_ms, &value);
    }

    fn store_memory(&self, key: String, ttl: Duration, value: ExecResult) {
        let mut cache = self.inner.cache.lock().expect("cache lock");
        if !cache.contains_key(&key)
            && cache.len() >= MAX_CACHE_ENTRIES
            && let Some(oldest) = cache
                .iter()
                .min_by_key(|(_, entry)| entry.expires)
                .map(|(key, _)| key.clone())
        {
            cache.remove(&oldest);
        }
        cache.insert(
            key,
            CacheEntry {
                expires: Instant::now() + ttl,
                value,
            },
        );
    }

    fn read_disk_cache(&self, key: &str) -> Option<(ExecResult, Duration)> {
        let hash = cache_hash(key);
        let path = self.cache_path(&hash)?;
        let metadata = std::fs::symlink_metadata(&path).ok()?;
        if !metadata.is_file()
            || metadata.uid() != unsafe { libc::geteuid() }
            || metadata.permissions().mode() & 0o077 != 0
            || metadata.len() > MAX_CACHE_FILE as u64
        {
            return None;
        }
        let mut bytes = Vec::new();
        File::open(&path)
            .ok()?
            .take((MAX_CACHE_FILE + 1) as u64)
            .read_to_end(&mut bytes)
            .ok()?;
        if bytes.len() > MAX_CACHE_FILE {
            return None;
        }
        let entry: DiskCacheEntry = serde_json::from_slice(&bytes).ok()?;
        let now = epoch_ms();
        if entry.version != 1
            || entry.key_hash != hash
            || now < entry.created_at_ms
            || entry.expires_at_ms <= now
        {
            let _ = std::fs::remove_file(path);
            return None;
        }
        Some((
            entry.value,
            Duration::from_millis(entry.expires_at_ms - now),
        ))
    }

    fn write_disk_cache(&self, key: &str, ttl_ms: u64, value: &ExecResult) {
        let hash = cache_hash(key);
        let Some(path) = self.cache_path(&hash) else {
            return;
        };
        let created_at_ms = epoch_ms();
        let entry = DiskCacheEntry {
            version: 1,
            key_hash: hash,
            created_at_ms,
            expires_at_ms: created_at_ms.saturating_add(ttl_ms),
            value: value.clone(),
        };
        let Ok(bytes) = serde_json::to_vec(&entry) else {
            return;
        };
        if bytes.len() > MAX_CACHE_FILE {
            return;
        }
        if !path.exists() {
            prune_disk_cache(path.parent().expect("cache version directory"));
        }
        let temporary = path.with_extension(format!("tmp-{}-{}", std::process::id(), epoch_ms()));
        let written = OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(&temporary)
            .and_then(|mut file| {
                use std::io::Write;
                file.write_all(&bytes)?;
                file.sync_all()
            })
            .and_then(|()| std::fs::rename(&temporary, &path));
        if written.is_err() {
            let _ = std::fs::remove_file(temporary);
        }
    }

    fn cache_path(&self, hash: &str) -> Option<PathBuf> {
        let root = self
            .inner
            .cache_dir
            .lock()
            .expect("cache directory lock")
            .clone()?;
        private_directory(&root).ok()?;
        let version = root.join("v1");
        private_directory(&version).ok()?;
        Some(version.join(format!("{hash}.json")))
    }
}

fn private_directory(path: &Path) -> std::io::Result<()> {
    let create = !path.exists();
    let mut builder = std::fs::DirBuilder::new();
    builder.recursive(true).mode(0o700);
    builder.create(path)?;
    let metadata = std::fs::symlink_metadata(path)?;
    if !metadata.is_dir() || metadata.uid() != unsafe { libc::geteuid() } {
        return Err(std::io::Error::new(
            std::io::ErrorKind::PermissionDenied,
            "cache directory is not owned",
        ));
    }
    if create {
        std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o700))?;
    } else if metadata.permissions().mode() & 0o077 != 0 {
        return Err(std::io::Error::new(
            std::io::ErrorKind::PermissionDenied,
            "cache directory is accessible by other users",
        ));
    }
    Ok(())
}

fn prune_disk_cache(directory: &Path) {
    let Ok(entries) = std::fs::read_dir(directory) else {
        return;
    };
    let mut files = entries
        .filter_map(|entry| entry.ok())
        .filter_map(|entry| {
            let path = entry.path();
            let metadata = entry.metadata().ok()?;
            (metadata.is_file() && path.extension().is_some_and(|value| value == "json"))
                .then_some((path, metadata.modified().ok()))
        })
        .collect::<Vec<_>>();
    files.sort_by_key(|(_, modified)| *modified);
    let remove = files
        .len()
        .saturating_add(1)
        .saturating_sub(MAX_CACHE_ENTRIES);
    for (path, _) in files.into_iter().take(remove) {
        let _ = std::fs::remove_file(path);
    }
}

fn cache_hash(key: &str) -> String {
    let mut value = 0xcbf29ce484222325_u64;
    for byte in key.as_bytes() {
        value ^= u64::from(*byte);
        value = value.wrapping_mul(0x100000001b3);
    }
    format!("{value:016x}")
}

fn epoch_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}

fn apply_environment(
    mut environment: Vec<(std::ffi::OsString, std::ffi::OsString)>,
    overrides: &[(String, Option<String>)],
) -> Vec<(std::ffi::OsString, std::ffi::OsString)> {
    for (name, value) in overrides {
        environment.retain(|(key, _)| key != std::ffi::OsStr::new(name));
        if let Some(value) = value {
            environment.push((name.into(), value.into()));
        }
    }
    environment.sort();
    environment
}

fn read_capped(mut reader: impl Read) -> Result<(String, bool), String> {
    let mut bytes = Vec::new();
    let mut buffer = [0_u8; 8 * 1024];
    let mut truncated = false;
    loop {
        let count = reader
            .read(&mut buffer)
            .map_err(|error| error.to_string())?;
        if count == 0 {
            break;
        }
        let remaining = MAX_EXEC_OUTPUT.saturating_sub(bytes.len());
        bytes.extend_from_slice(&buffer[..count.min(remaining)]);
        truncated |= count > remaining;
    }
    Ok((String::from_utf8_lossy(&bytes).into_owned(), truncated))
}

fn raise<'gc>(ctx: Context<'gc>, message: impl std::fmt::Display) -> Error<'gc> {
    Value::String(ctx.intern(message.to_string().as_bytes())).into()
}

fn valid_env_name(name: &str) -> bool {
    let mut bytes = name.bytes();
    matches!(bytes.next(), Some(byte) if byte == b'_' || byte.is_ascii_alphabetic())
        && bytes.all(|byte| byte == b'_' || byte.is_ascii_alphanumeric())
}

#[cfg(test)]
mod tests {
    use super::{ExecResult, HostState, MAX_CACHE_ENTRIES, apply_environment, valid_env_name};
    use std::os::unix::fs::PermissionsExt;
    use std::time::{SystemTime, UNIX_EPOCH};

    #[test]
    fn validates_environment_names() {
        assert!(valid_env_name("PIXY_TEST_1"));
        assert!(!valid_env_name("PIXY-TEST"));
    }

    #[test]
    fn provider_cache_has_a_fixed_entry_bound() {
        let host = HostState::default();
        let result = ExecResult {
            status: 0,
            stdout: String::new(),
            stderr: String::new(),
            timed_out: false,
            truncated: false,
        };
        for index in 0..(MAX_CACHE_ENTRIES + 32) {
            host.store_cache(index.to_string(), 1_000, result.clone());
        }
        assert_eq!(
            host.inner.cache.lock().expect("cache lock").len(),
            MAX_CACHE_ENTRIES
        );
    }

    #[test]
    fn request_environment_overrides_and_unsets_inherited_values() {
        let inherited = vec![("KEEP".into(), "old".into()), ("DROP".into(), "old".into())];
        let overrides = vec![
            ("KEEP".into(), Some("new".into())),
            ("DROP".into(), None),
            ("ADD".into(), Some("value".into())),
        ];
        let environment = apply_environment(inherited, &overrides);
        assert_eq!(
            environment,
            vec![
                ("ADD".into(), "value".into()),
                ("KEEP".into(), "new".into())
            ]
        );
    }

    #[test]
    fn provider_cache_persists_in_private_versioned_files() {
        let unique = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("time")
            .as_nanos();
        let root = std::env::temp_dir().join(format!("pixy-cache-{}-{unique}", std::process::id()));
        let cache_dir = root.join("cache");
        let result = ExecResult {
            status: 0,
            stdout: "cached".into(),
            stderr: String::new(),
            timed_out: false,
            truncated: false,
        };
        let first = HostState::default();
        first.set_cache_dir(cache_dir.clone());
        first.store_cache("key".into(), 1_000, result);
        let second = HostState::default();
        second.set_cache_dir(cache_dir.clone());
        assert_eq!(
            second.cached("key", 1_000).expect("disk cache").stdout,
            "cached"
        );
        assert_eq!(
            std::fs::metadata(&cache_dir)
                .expect("cache metadata")
                .permissions()
                .mode()
                & 0o777,
            0o700
        );
        let file = std::fs::read_dir(cache_dir.join("v1"))
            .expect("version cache")
            .next()
            .expect("cache entry")
            .expect("cache file")
            .path();
        assert_eq!(
            std::fs::metadata(file)
                .expect("entry metadata")
                .permissions()
                .mode()
                & 0o777,
            0o600
        );
        for index in 0..(MAX_CACHE_ENTRIES + 8) {
            first.store_cache(
                format!("disk-{index}"),
                1_000,
                ExecResult {
                    status: 0,
                    stdout: index.to_string(),
                    stderr: String::new(),
                    timed_out: false,
                    truncated: false,
                },
            );
        }
        assert!(
            std::fs::read_dir(cache_dir.join("v1"))
                .expect("bounded cache")
                .count()
                <= MAX_CACHE_ENTRIES
        );
        std::fs::remove_dir_all(root).expect("cleanup");
    }
}
