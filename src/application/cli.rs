use super::engine::Engine;
use crate::model::context::RenderContext;
use crate::model::error::{PixyError, Result};
use crate::model::output::{LineTarget, RenderMode, RenderOutput, RenderRequest};
use crate::runtime::assets;
use crate::runtime::config::{ConfigSource, Paths};
use crate::runtime::scheduler::{Latest, Scheduler};
use serde_json::Value;
use std::collections::BTreeMap;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::Arc;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

pub fn run() -> Result<()> {
    let mut args = std::env::args().skip(1).collect::<Vec<_>>();
    if args.is_empty()
        || matches!(
            args.first().map(String::as_str),
            Some("--help" | "-h" | "help")
        )
    {
        print_help();
        return Ok(());
    }
    let command = args.remove(0);
    match command.as_str() {
        "render" => render_command(args),
        "list" => list_command(args),
        "check" => check_command(args),
        "init" => init_command(args),
        "stream" => stream_command(args),
        "pack" => pack_command(args),
        "serve" => serve_command(args),
        "__bench" => bench_command(args),
        value if selector_list(value).is_ok() => {
            args.insert(0, value.to_string());
            render_command(args)
        }
        _ => Err(PixyError::Usage(format!(
            "unknown command or selector '{command}'"
        ))),
    }
}

fn bench_command(args: Vec<String>) -> Result<()> {
    if args.len() > 2 {
        return Err(PixyError::Usage(
            "usage: pixy __bench <cold|query|provider|queue|compat> [count]".into(),
        ));
    }
    let count = args
        .get(1)
        .map(|value| parse_number(value, "count"))
        .transpose()?;
    match args.first().map(String::as_str) {
        Some("cold") => bench_cold(count.unwrap_or(500)),
        Some("query") => bench_query(count.unwrap_or(10_000)),
        Some("provider") => bench_provider(count.unwrap_or(100)),
        Some("queue") => bench_queue(count.unwrap_or(100_000)),
        Some("compat") => bench_compat(count.unwrap_or(500)),
        _ => Err(PixyError::Usage(
            "usage: pixy __bench <cold|query|provider|queue|compat> [count]".into(),
        )),
    }
}

fn bench_compat(requests: usize) -> Result<()> {
    let root = benchmark_root("compat");
    std::fs::create_dir_all(&root).map_err(transport)?;
    let config = root.join("init.lua");
    std::fs::write(&config, include_str!("../../examples/hexe-oslo.lua")).map_err(transport)?;
    let mut context = RenderContext::default();
    for (name, value) in [
        ("cwd", "/work/project"),
        ("status", "7"),
        ("language", "lua"),
        ("vimode", "N"),
    ] {
        context
            .values
            .insert(name.into(), Value::String(value.into()));
    }
    for name in ["hostname", "distro", "sudo", "container", "git_branch"] {
        context.values.insert(name.into(), Value::Bool(false));
    }
    context
        .values
        .insert("username".into(), Value::String("user".into()));
    context
        .values
        .insert("scratch_count".into(), Value::Number(0.into()));
    context
        .values
        .insert("git_status".into(), Value::Bool(false));
    context
        .values
        .insert("git_status_text".into(), Value::Bool(false));
    let request = RenderRequest {
        select: vec!["prompt.left".into()],
        target: Some(LineTarget::Plain),
        width: 80,
        now_ms: Some(0),
        context,
        ..RenderRequest::default()
    };
    let mut cold = Vec::with_capacity(requests);
    for _ in 0..requests {
        let started = Instant::now();
        load_engine(Some(&config))?.render(request.clone())?;
        cold.push(started.elapsed().as_nanos());
    }
    let engine = load_engine(Some(&config))?;
    let mut query = Vec::with_capacity(requests);
    for _ in 0..requests {
        let started = Instant::now();
        engine.render(request.clone())?;
        query.push(started.elapsed().as_nanos());
    }
    let segment_request = RenderRequest {
        select: vec!["prompt.left.username".into()],
        ..request
    };
    let mut segment = Vec::with_capacity(requests);
    for _ in 0..requests {
        let started = Instant::now();
        engine.render(segment_request.clone())?;
        segment.push(started.elapsed().as_nanos());
    }
    std::fs::remove_dir_all(root).map_err(transport)?;
    println!("compat_cold_p95_ns={}", percentile_95(&mut cold));
    println!("compat_query_p95_ns={}", percentile_95(&mut query));
    println!("compat_segment_p95_ns={}", percentile_95(&mut segment));
    Ok(())
}

fn bench_queue(updates: usize) -> Result<()> {
    let queue = Latest::default();
    for update in 0..updates {
        queue.submit(update);
    }
    let pending = usize::from(queue.take().is_some()) + usize::from(queue.take().is_some());
    println!("pending_outputs={pending}");
    Ok(())
}

fn bench_provider(requests: usize) -> Result<()> {
    let root = benchmark_root("provider");
    std::fs::create_dir_all(&root).map_err(transport)?;
    let config = root.join("init.lua");
    std::fs::write(&config, "local pixy=require('pixy'); return pixy.config({zones={provider=pixy.zone({pixy.segment('status',function() local value=pixy.host.exec({'true'},{timeout_ms=100}); return tostring(value.status) end)})}})").map_err(transport)?;
    let engine = load_engine(Some(&config))?;
    let request = RenderRequest {
        select: vec!["provider".into()],
        target: Some(LineTarget::Plain),
        now_ms: Some(0),
        ..RenderRequest::default()
    };
    let mut samples = Vec::with_capacity(requests);
    for _ in 0..requests {
        let started = Instant::now();
        engine.render(request.clone())?;
        samples.push(started.elapsed().as_nanos());
    }
    let p95 = percentile_95(&mut samples);
    std::fs::remove_dir_all(root).map_err(transport)?;
    println!("provider_exec_p95_ns={p95}");
    Ok(())
}

fn bench_cold(launches: usize) -> Result<()> {
    let root = benchmark_root("cold");
    std::fs::create_dir_all(&root).map_err(transport)?;
    let config = root.join("init.lua");
    std::fs::write(&config, "local pixy=require('pixy'); return pixy.config({zones={literal=pixy.zone({pixy.segment('text',function() return 'pixy' end)})}})").map_err(transport)?;
    let executable = std::env::current_exe().map_err(transport)?;
    let launch = || -> Result<()> {
        let status = Command::new(&executable)
            .args([
                "render",
                "literal",
                "--target",
                "plain",
                "--width",
                "80",
                "--config",
                config.to_str().expect("config path"),
            ])
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status()
            .map_err(transport)?;
        if !status.success() {
            return Err(PixyError::Transport("cold benchmark child failed".into()));
        }
        Ok(())
    };
    for _ in 0..100 {
        launch()?;
    }
    let mut samples = Vec::with_capacity(launches);
    for _ in 0..launches {
        let started = Instant::now();
        launch()?;
        samples.push(started.elapsed().as_nanos());
    }
    let p95 = percentile_95(&mut samples);
    std::fs::remove_dir_all(root).map_err(transport)?;
    println!("cold_p95_ns={p95}");
    Ok(())
}

fn bench_query(requests: usize) -> Result<()> {
    let root = benchmark_root("query");
    std::fs::create_dir_all(&root).map_err(transport)?;
    let config = root.join("init.lua");
    std::fs::write(&config, "local pixy=require('pixy'); return pixy.config({zones={literal=pixy.zone({pixy.segment('text',function() return 'pixy' end)})}})").map_err(transport)?;
    let engine = load_engine(Some(&config))?;
    let request = RenderRequest {
        select: vec!["literal".into()],
        target: Some(LineTarget::Plain),
        now_ms: Some(0),
        ..RenderRequest::default()
    };
    let mut samples = Vec::with_capacity(requests);
    for _ in 0..requests {
        let started = Instant::now();
        engine.render(request.clone())?;
        samples.push(started.elapsed().as_nanos());
    }
    let p95 = percentile_95(&mut samples);
    std::fs::remove_dir_all(root).map_err(transport)?;
    println!("query_p95_ns={p95}");
    println!("lua_memory_limit_bytes={}", 32 * 1024 * 1024);
    Ok(())
}

fn percentile_95(samples: &mut [u128]) -> u128 {
    samples.sort_unstable();
    let index = samples
        .len()
        .saturating_mul(95)
        .div_ceil(100)
        .saturating_sub(1);
    samples.get(index).copied().unwrap_or(0)
}

fn benchmark_root(name: &str) -> PathBuf {
    std::env::temp_dir().join(format!(
        "pixy-bench-{name}-{}-{}",
        std::process::id(),
        unix_time_ms()
    ))
}

fn render_command(args: Vec<String>) -> Result<()> {
    let options = parse_render(args)?;
    let output = render_once(options.config.as_deref(), options.request)?;
    write_output(&output, options.newline)
}

fn list_command(args: Vec<String>) -> Result<()> {
    let config = parse_config_only(args)?;
    let engine = load_engine(config.as_deref())?;
    for name in engine.list()? {
        println!("{name}");
    }
    Ok(())
}

fn check_command(args: Vec<String>) -> Result<()> {
    let config = parse_config_only(args)?;
    let engine = load_engine(config.as_deref())?;
    let (zones, segments) = engine.counts()?;
    println!(
        "ok {} ({zones} zones, {segments} segments)",
        engine.source_name
    );
    Ok(())
}

fn serve_command(mut args: Vec<String>) -> Result<()> {
    let mut socket = None;
    if let Some(index) = args.iter().position(|value| value == "--socket") {
        if index + 1 >= args.len() {
            return Err(PixyError::Usage("--socket requires a path".into()));
        }
        socket = Some(PathBuf::from(args.remove(index + 1)));
        args.remove(index);
    }
    let config = parse_config_only(args)?;
    super::serve::serve(socket, config.as_deref())
}

fn init_command(args: Vec<String>) -> Result<()> {
    if args.len() != 1 {
        return Err(PixyError::Usage(
            "usage: pixy init <bash|zsh|fish|oslo|hexe-oslo>".into(),
        ));
    }
    let output = match args[0].as_str() {
        "bash" => BASH_INIT,
        "zsh" => ZSH_INIT,
        "fish" => FISH_INIT,
        "oslo" => include_str!("../../examples/oslo.lua"),
        "hexe-oslo" => include_str!("../../examples/hexe-oslo.lua"),
        shell => {
            return Err(PixyError::Usage(format!(
                "unsupported integration '{shell}'"
            )));
        }
    };
    print!("{output}");
    Ok(())
}

fn stream_command(mut args: Vec<String>) -> Result<()> {
    let mut fps = 12_u32;
    let mut duration_ms = 1_000_u64;
    take_numeric_option(&mut args, "--fps", |value| {
        let value = value
            .try_into()
            .map_err(|_| PixyError::Usage("fps is too large".into()))?;
        if !(1..=1_000).contains(&value) {
            return Err(PixyError::Usage("fps must be between 1 and 1000".into()));
        }
        fps = value;
        Ok(())
    })?;
    take_numeric_option(&mut args, "--duration", |value| {
        if value > 24 * 60 * 60 * 1_000 {
            return Err(PixyError::Usage(
                "stream duration cannot exceed 24 hours".into(),
            ));
        }
        duration_ms = value;
        Ok(())
    })?;
    let mut options = parse_render(args)?;
    let engine = load_engine(options.config.as_deref())?;
    let started = Instant::now();
    let mut scheduler = Scheduler::new(fps);
    let mut previous = None;
    let pending = Arc::new(Latest::default());
    let writer_pending = Arc::clone(&pending);
    let newline = options.newline;
    let writer = std::thread::spawn(move || -> Result<()> {
        let mut first = true;
        let mut rewind = String::new();
        while let Some((payload, next_rewind)) = writer_pending.take_wait() {
            if !first {
                print!("{rewind}");
            }
            print!("{payload}");
            std::io::stdout().flush().map_err(transport)?;
            rewind = next_rewind;
            first = false;
        }
        if newline {
            println!();
        }
        Ok(())
    });
    let render_result = (|| -> Result<()> {
        let mut first_frame = true;
        loop {
            if !first_frame && started.elapsed() >= Duration::from_millis(duration_ms) {
                break;
            }
            first_frame = false;
            let rendered_at_ms = unix_time_ms();
            options.request.now_ms = Some(rendered_at_ms);
            let (output, rewind) = engine.render_stream(options.request.clone())?;
            let next_frame_ms = output.next_frame_ms();
            let payload = output_payload(&output)?;
            if previous.as_deref() != Some(payload.as_str()) {
                pending.submit((payload.clone(), rewind));
                previous = Some(payload);
            }
            if started.elapsed() >= Duration::from_millis(duration_ms) {
                break;
            }
            let remaining = Duration::from_millis(duration_ms).saturating_sub(started.elapsed());
            if next_frame_ms.is_none() {
                std::thread::sleep(remaining);
                break;
            }
            scheduler.wait_until_capped(next_frame_ms, rendered_at_ms, Some(remaining));
        }
        Ok(())
    })();
    pending.close();
    let writer_result = writer
        .join()
        .map_err(|_| PixyError::Transport("stream writer failed".into()))?;
    render_result?;
    writer_result
}

fn pack_command(mut args: Vec<String>) -> Result<()> {
    if args.is_empty() {
        return Err(PixyError::Usage(
            "usage: pixy pack <build|check|list>".into(),
        ));
    }
    let command = args.remove(0);
    match command.as_str() {
        "build" => pack_build(args),
        "check" => {
            if args.len() != 1 {
                return Err(PixyError::Usage("usage: pixy pack check <file>".into()));
            }
            let pack = assets::load(Path::new(&args[0]))?;
            println!("ok {} items", pack.items.len());
            Ok(())
        }
        "list" => {
            if args.is_empty() {
                return pack_list_installed();
            }
            let path = if args.len() == 1 {
                PathBuf::from(&args[0])
            } else {
                return Err(PixyError::Usage("usage: pixy pack list <file>".into()));
            };
            let pack = assets::load(&path)?;
            println!("source\t{}", pack.source);
            println!("license\t{}", pack.license);
            for (name, item) in pack.items {
                println!("{name}\t{}\t{}", item.raw_size, item.checksum);
            }
            Ok(())
        }
        other => Err(PixyError::Usage(format!("unknown pack command '{other}'"))),
    }
}

fn pack_list_installed() -> Result<()> {
    for pack in assets::embedded_packs()? {
        println!("{}\t{}\t{} (embedded)", pack.name, pack.items, pack.source);
    }
    let directory = Paths::discover()?.data_dir;
    let Ok(entries) = std::fs::read_dir(directory) else {
        return Ok(());
    };
    let mut paths = entries
        .filter_map(|entry| entry.ok().map(|entry| entry.path()))
        .filter(|path| {
            path.extension()
                .is_some_and(|extension| extension == "pixypack")
        })
        .collect::<Vec<_>>();
    paths.sort();
    for path in paths {
        let pack = assets::load(&path)?;
        let name = path
            .file_stem()
            .and_then(|value| value.to_str())
            .unwrap_or("?");
        println!("{name}\t{}\t{}", pack.items.len(), pack.source);
    }
    Ok(())
}

fn pack_build(args: Vec<String>) -> Result<()> {
    let directory = args
        .first()
        .ok_or_else(|| PixyError::Usage("pack build requires a directory".into()))?;
    let mut output = None;
    let mut source = "local".to_string();
    let mut license = "unknown".to_string();
    let mut attribution = String::new();
    let mut index = 1;
    while index < args.len() {
        match args[index].as_str() {
            "--output" => output = Some(PathBuf::from(value_after(&args, &mut index, "--output")?)),
            "--source" => source = value_after(&args, &mut index, "--source")?.to_string(),
            "--license" => license = value_after(&args, &mut index, "--license")?.to_string(),
            "--attribution" => {
                attribution = value_after(&args, &mut index, "--attribution")?.to_string()
            }
            other => return Err(PixyError::Usage(format!("unknown pack option '{other}'"))),
        }
        index += 1;
    }
    let output = output.ok_or_else(|| PixyError::Usage("pack build requires --output".into()))?;
    assets::build(Path::new(directory), &output, source, license, attribution)
}

struct RenderOptions {
    request: RenderRequest,
    config: Option<PathBuf>,
    newline: bool,
}

fn parse_render(args: Vec<String>) -> Result<RenderOptions> {
    let mut request = RenderRequest::default();
    let mut config = None;
    let mut newline = false;
    let mut context_json = None;
    let mut request_json = None;
    let mut sets = BTreeMap::new();
    let mut index = 0;
    if args.first().is_some_and(|value| !value.starts_with('-')) {
        request.select = selector_list(&args[0])?;
        index = 1;
    }
    while index < args.len() {
        let option = &args[index];
        let (name, inline) = option
            .split_once('=')
            .map_or((option.as_str(), None), |(name, value)| (name, Some(value)));
        let mut next_value = |label: &str| -> Result<String> {
            if let Some(value) = inline {
                return Ok(value.to_string());
            }
            index += 1;
            args.get(index)
                .cloned()
                .ok_or_else(|| PixyError::Usage(format!("{label} requires a value")))
        };
        match name {
            "--mode" => request.mode = parse_mode(&next_value(name)?)?,
            "--target" => request.target = Some(parse_target(&next_value(name)?)?),
            "--width" => request.width = parse_number(&next_value(name)?, name)?,
            "--height" => request.height = parse_number(&next_value(name)?, name)?,
            "--now-ms" => request.now_ms = Some(parse_number(&next_value(name)?, name)?),
            "--set" => {
                let value = next_value(name)?;
                let (key, value) = value
                    .split_once('=')
                    .ok_or_else(|| PixyError::Usage("--set requires key=value".into()))?;
                if key.is_empty() {
                    return Err(PixyError::Usage("--set key cannot be empty".into()));
                }
                sets.insert(key.to_string(), scalar(value));
            }
            "--context-json" => context_json = Some(next_value(name)?),
            "--context-file" => {
                let path = next_value(name)?;
                context_json = Some(std::fs::read_to_string(&path).map_err(|error| {
                    PixyError::Usage(format!("failed to read context file {path}: {error}"))
                })?)
            }
            "--request" => {
                let path = next_value(name)?;
                let mut value = String::new();
                if path == "-" {
                    std::io::stdin()
                        .read_to_string(&mut value)
                        .map_err(|error| {
                            PixyError::Usage(format!("failed to read request stdin: {error}"))
                        })?;
                } else {
                    value = std::fs::read_to_string(&path).map_err(|error| {
                        PixyError::Usage(format!("failed to read request file {path}: {error}"))
                    })?;
                }
                request_json = Some(value);
            }
            "--config" => config = Some(PathBuf::from(next_value(name)?)),
            "--ignore-missing" => request.ignore_missing = true,
            "--newline" => newline = true,
            "--help" | "-h" => {
                print_render_help();
                std::process::exit(0);
            }
            other => return Err(PixyError::Usage(format!("unknown render option '{other}'"))),
        }
        index += 1;
    }
    request.context.values.extend(sets);
    if let Some(json) = context_json {
        request.context = serde_json::from_str::<RenderContext>(&json)
            .map_err(|error| PixyError::Usage(format!("invalid context JSON: {error}")))?;
    }
    if let Some(json) = request_json {
        request = serde_json::from_str::<RenderRequest>(&json)
            .map_err(|error| PixyError::Usage(format!("invalid request JSON: {error}")))?;
    }
    request.validate()?;
    Ok(RenderOptions {
        request,
        config,
        newline,
    })
}

fn render_once(config: Option<&Path>, request: RenderRequest) -> Result<RenderOutput> {
    load_engine(config)?.render(request)
}

fn load_engine(config: Option<&Path>) -> Result<Engine> {
    let paths = Paths::discover()?;
    let source = ConfigSource::load(config, &paths)?;
    Engine::load(source, &paths)
}

fn write_output(output: &RenderOutput, newline: bool) -> Result<()> {
    let payload = output_payload(output)?;
    if newline {
        println!("{payload}");
    } else {
        print!("{payload}");
    }
    std::io::stdout().flush().map_err(transport)
}

fn output_payload(output: &RenderOutput) -> Result<String> {
    match output {
        RenderOutput::Line { text, .. } => Ok(text.clone()),
        RenderOutput::Run { .. } => {
            serde_json::to_string(output).map_err(|error| PixyError::Render(error.to_string()))
        }
        RenderOutput::Surface { ansi, .. } => Ok(ansi.clone()),
    }
}

fn selector_list(value: &str) -> Result<Vec<String>> {
    let selectors = value.split(',').map(str::to_string).collect::<Vec<_>>();
    if selectors.is_empty()
        || selectors
            .iter()
            .any(|value| !crate::model::output::valid_selector(value))
    {
        return Err(PixyError::Usage(format!("invalid selector list '{value}'")));
    }
    Ok(selectors)
}

fn parse_config_only(args: Vec<String>) -> Result<Option<PathBuf>> {
    match args.as_slice() {
        [] => Ok(None),
        [flag, value] if flag == "--config" => Ok(Some(PathBuf::from(value))),
        _ => Err(PixyError::Usage("only --config PATH is accepted".into())),
    }
}

fn value_after<'a>(args: &'a [String], index: &mut usize, option: &str) -> Result<&'a str> {
    *index += 1;
    args.get(*index)
        .map(String::as_str)
        .ok_or_else(|| PixyError::Usage(format!("{option} requires a value")))
}

fn take_numeric_option<F>(args: &mut Vec<String>, name: &str, mut assign: F) -> Result<()>
where
    F: FnMut(u64) -> Result<()>,
{
    let mut index = 0;
    let inline = format!("{name}=");
    while index < args.len() {
        if args[index] == name {
            if index + 1 >= args.len() {
                return Err(PixyError::Usage(format!("{name} requires a value")));
            }
            let value = parse_number::<u64>(&args[index + 1], name)?;
            assign(value)?;
            args.drain(index..=index + 1);
        } else if let Some(value) = args[index].strip_prefix(&inline) {
            let value = parse_number::<u64>(value, name)?;
            assign(value)?;
            args.remove(index);
        } else {
            index += 1;
        }
    }
    Ok(())
}

fn parse_mode(value: &str) -> Result<RenderMode> {
    match value {
        "line" => Ok(RenderMode::Line),
        "run" => Ok(RenderMode::Run),
        "surface" => Ok(RenderMode::Surface),
        _ => Err(PixyError::Usage(format!("invalid mode '{value}'"))),
    }
}

fn parse_target(value: &str) -> Result<LineTarget> {
    match value {
        "plain" => Ok(LineTarget::Plain),
        "ansi" => Ok(LineTarget::Ansi),
        "bash" => Ok(LineTarget::Bash),
        "zsh" => Ok(LineTarget::Zsh),
        _ => Err(PixyError::Usage(format!("invalid target '{value}'"))),
    }
}

/// A `--set` value, as the number, boolean or null it spells, else as its text.
///
/// A shell writes `--set jobs=$jobs`, and a segment comparing that against a
/// number needs a number. An empty value is absent rather than an empty string,
/// so an unset shell variable reads as nil.
fn scalar(value: &str) -> Value {
    if value.is_empty() {
        return Value::Null;
    }
    match serde_json::from_str::<Value>(value) {
        Ok(parsed) if !parsed.is_string() => parsed,
        _ => Value::String(value.to_string()),
    }
}

fn parse_number<T>(value: &str, option: &str) -> Result<T>
where
    T: std::str::FromStr,
{
    value
        .parse()
        .map_err(|_| PixyError::Usage(format!("{option} requires a number")))
}

fn unix_time_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}
fn transport(error: std::io::Error) -> PixyError {
    PixyError::Transport(error.to_string())
}

fn print_help() {
    print!("{HELP}");
}

fn print_render_help() {
    println!(
        "pixy render <zone[.segment][,...]> [--mode line|run|surface] [--target plain|ansi|bash|zsh]"
    );
}

const HELP: &str = "pixy - Lua terminal painter\n\nUsage:\n  pixy render <zone[.segment][,...]> [options]\n  pixy <zone[.segment][,...]> [options]\n  pixy list [--config PATH]\n  pixy check [--config PATH]\n  pixy init <bash|zsh|fish|oslo|hexe-oslo>\n  pixy stream <zone[.segment][,...]> [--fps N] [--duration MS]\n  pixy pack build <directory> --output <file>\n  pixy pack check <file>\n  pixy pack list [<file>]\n  pixy serve [--socket PATH] [--config PATH]\n";

const BASH_INIT: &str = r#"__pixy_prompt_command() {
  local pixy_status=${__pixy_last_status:-0}
  local pixy_duration=0
  local pixy_jobs=$(( $(jobs -p | wc -l) ))
  __pixy_prompt_active=1
  if [[ -n ${__pixy_started_seconds:-} ]]; then
    pixy_duration=$(( (SECONDS - __pixy_started_seconds) * 1000 ))
  fi
  PS1="$(command pixy render prompt.left --target bash --set status="$pixy_status" --set duration_ms="$pixy_duration" --set jobs="$pixy_jobs" --set language="${PIXY_LANGUAGE:-}" --set vimode="${PIXY_VIMODE:-}")"
}
__pixy_prompt_finish() {
  __pixy_prompt_active=0
  __pixy_command_running=0
}
__pixy_preexec() {
  local pixy_status=$?
  if [[ $BASH_COMMAND == __pixy_prompt_command* ]]; then
    __pixy_last_status=$pixy_status
    __pixy_prompt_active=1
    return
  fi
  if [[ ${__pixy_prompt_active:-0} -eq 0 && ${__pixy_command_running:-0} -eq 0 ]]; then
    __pixy_started_seconds=$SECONDS
    __pixy_command_running=1
  fi
}
trap '__pixy_preexec' DEBUG
PROMPT_COMMAND="__pixy_prompt_command${PROMPT_COMMAND:+;$PROMPT_COMMAND};__pixy_prompt_finish"
"#;

const ZSH_INIT: &str = r#"zmodload zsh/datetime
autoload -Uz add-zsh-hook
typeset -gF __pixy_started_at=0
__pixy_preexec() {
  __pixy_started_at=$EPOCHREALTIME
}
__pixy_precmd() {
  local pixy_status=$?
  local pixy_duration=0
  if (( __pixy_started_at > 0 )); then
    pixy_duration=$(( (EPOCHREALTIME - __pixy_started_at) * 1000 ))
  fi
  PROMPT="$(command pixy render prompt.left --target zsh --set status="$pixy_status" --set duration_ms="$pixy_duration" --set jobs="${#jobstates}" --set language="${PIXY_LANGUAGE:-}" --set vimode="${KEYMAP:-}")"
  RPROMPT="$(command pixy render prompt.right --target zsh --set status="$pixy_status" --set duration_ms="$pixy_duration" --set jobs="${#jobstates}" --set language="${PIXY_LANGUAGE:-}" --set vimode="${KEYMAP:-}")"
}
add-zsh-hook preexec __pixy_preexec
add-zsh-hook precmd __pixy_precmd
"#;

const FISH_INIT: &str = r#"set -g __pixy_last_command ''
function __pixy_preexec --on-event fish_preexec
  set -g __pixy_last_command $argv
end
function fish_prompt
  set -g __pixy_status $status
  set -l pixy_language ''
  set -q PIXY_LANGUAGE; and set pixy_language "$PIXY_LANGUAGE"
  command pixy render prompt.left --target ansi --set status=$__pixy_status --set duration_ms="$CMD_DURATION" --set jobs=(count (jobs -p)) --set language="$pixy_language" --set vimode="$fish_bind_mode" --set "last_command=$__pixy_last_command"
end
function fish_right_prompt
  set -l pixy_language ''
  set -q PIXY_LANGUAGE; and set pixy_language "$PIXY_LANGUAGE"
  command pixy render prompt.right --target ansi --set status="$__pixy_status" --set duration_ms="$CMD_DURATION" --set jobs=(count (jobs -p)) --set language="$pixy_language" --set vimode="$fish_bind_mode"
end
"#;
