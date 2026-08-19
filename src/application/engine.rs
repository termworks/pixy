use crate::model::context::RenderContext;
use crate::model::error::{PixyError, Result};
use crate::model::output::{RenderOutput, RenderRequest, Run};
use crate::runtime::config::{ConfigSource, Paths};
use crate::runtime::host::HostState;
use luna::{
    Closure, Context, Executor, FromValue, Fuel, Function, Lua, StashedExecutor, StashedFunction,
    StashedTable, Table, Value,
};
use std::cell::RefCell;
use std::collections::BTreeSet;
use std::path::PathBuf;
use std::time::{Duration, Instant};

const MEMORY_LIMIT: usize = 32 * 1024 * 1024;
/// Ceiling on Lua execution per render.
const RENDER_DEADLINE: Duration = Duration::from_millis(100);
/// Ceiling on compiling the bundled modules and evaluating a configuration.
const LOAD_DEADLINE: Duration = Duration::from_millis(250);
const OUTPUT_LIMIT: usize = 1024 * 1024;
const FUEL_PER_SLICE: i32 = 4096;
const MAX_RUNS: usize = 262_144;
const MAX_REGIONS: usize = 4_096;
const MAX_SEGMENTS: usize = 65_536;

const MODULES: &[(&str, &str)] = &[
    ("pixy.nodes", include_str!("../../lua/pixy/nodes.lua")),
    ("pixy.layout", include_str!("../../lua/pixy/layout.lua")),
    ("pixy.style", include_str!("../../lua/pixy/style.lua")),
    ("pixy.ansi", include_str!("../../lua/pixy/ansi.lua")),
    ("pixy.encode", include_str!("../../lua/pixy/encode.lua")),
    ("pixy.animate", include_str!("../../lua/pixy/animate.lua")),
    ("pixy.sprite", include_str!("../../lua/pixy/sprite.lua")),
    (
        "pixy.segments.shell",
        include_str!("../../lua/pixy/segments/shell.lua"),
    ),
    (
        "pixy.segments.git",
        include_str!("../../lua/pixy/segments/git.lua"),
    ),
    (
        "pixy.segments.system",
        include_str!("../../lua/pixy/segments/system.lua"),
    ),
    ("pixy", include_str!("../../lua/pixy/init.lua")),
];

const MODULE_LOADER: &str = r#"
local preload = package.searchers[1]
local compile = load
local function pixy_searcher(name)
  if type(name) ~= "string" or not name:match("^[%w_][%w_.-]*$") then
    return "\n\tinvalid Pixy Lua module name " .. tostring(name)
  end
  local relative = name:gsub("%.", "/")
  local candidates = {
    relative .. ".lua",
    relative .. "/init.lua",
    "lua/" .. relative .. ".lua",
    "lua/" .. relative .. "/init.lua",
  }
  for _, candidate in ipairs(candidates) do
    local ok, source = pcall(__pixy_host.read, candidate)
    if ok and source ~= nil then
      local loader, message = compile(source, "@" .. candidate, "t")
      if not loader then error(message, 0) end
      return loader, candidate
    end
  end
  return "\n\tno Pixy Lua module '" .. name .. "'"
end
package.searchers = {preload, pixy_searcher}
package.path = ""
"#;

pub struct Engine {
    lua: RefCell<Lua>,
    config: StashedTable,
    render: StashedFunction,
    executor: StashedExecutor,
    host: HostState,
    pub source_name: String,
}

impl Engine {
    pub fn load(source: ConfigSource, paths: &Paths) -> Result<Self> {
        if source.source.len() > crate::runtime::config::MAX_CONFIG_SIZE {
            return Err(PixyError::Config(format!("{} exceeds 1 MiB", source.name)));
        }
        let mut lua = Lua::core();
        lua.load_package();
        lua.set_memory_limit(Some(MEMORY_LIMIT));
        let host = HostState::default();
        host.set_roots(vec![
            source.directory.clone(),
            paths.data_dir.clone(),
            PathBuf::from("/proc"),
            PathBuf::from("/sys"),
        ]);
        host.set_data_dir(paths.data_dir.clone());
        host.set_cache_dir(paths.cache_dir.clone());
        lua.try_enter(|ctx| host.install(ctx))
            .map_err(render_error)?;

        let deadline = Instant::now() + LOAD_DEADLINE;
        host.begin_render();
        install_modules(&mut lua, deadline, &host)?;

        let bootstrap = lua
            .try_enter(|ctx| {
                let require: Function = ctx.get_global("require")?;
                Ok(ctx.stash(Executor::start(ctx, require, "pixy")))
            })
            .map_err(render_error)?;
        run(&mut lua, &bootstrap, deadline, Some(&host)).map_err(render_error)?;
        let render = lua
            .try_enter(|ctx| {
                let pixy: Table = ctx.fetch(&bootstrap).take_result::<Table>(ctx)??;
                Ok(ctx.stash(pixy.get::<_, Function>(ctx, "_render")?))
            })
            .map_err(render_error)?;

        let chunk = lua
            .try_enter(|ctx| {
                let closure = Closure::load(ctx, Some(&source.name), source.source.as_bytes())?;
                Ok(ctx.stash(Executor::start(ctx, closure.into(), ())))
            })
            .map_err(|error| PixyError::Config(format!("{}: {error}", source.name)))?;
        run(&mut lua, &chunk, deadline, Some(&host))
            .map_err(|error| PixyError::Config(format!("{}: {error}", source.name)))?;
        let config = lua
            .try_enter(|ctx| Ok(ctx.stash(ctx.fetch(&chunk).take_result::<Table>(ctx)??)))
            .map_err(|error| PixyError::Config(format!("{}: {error}", source.name)))?;

        lua.try_enter(|ctx| {
            validate_config(ctx, ctx.fetch(&config), &source.name, deadline)
                .map_err(|error| Value::String(ctx.intern(error.to_string().as_bytes())).into())
        })
        .map_err(|error| PixyError::Config(error.to_string()))?;

        let executor = lua.enter(|ctx| ctx.stash(Executor::new(ctx)));
        lua.gc_collect();
        Ok(Self {
            lua: RefCell::new(lua),
            config,
            render,
            executor,
            host,
            source_name: source.name,
        })
    }

    pub fn bundled() -> Result<Self> {
        let paths = Paths::discover()?;
        let source = ConfigSource::load(None, &paths)?;
        Self::load(source, &paths)
    }

    pub fn render(&self, request: RenderRequest) -> Result<RenderOutput> {
        self.render_inner(request).map(|(output, _)| output)
    }

    pub(crate) fn render_stream(&self, request: RenderRequest) -> Result<(RenderOutput, String)> {
        self.render_inner(request)
    }

    fn render_inner(&self, mut request: RenderRequest) -> Result<(RenderOutput, String)> {
        request.validate()?;
        if request.now_ms.is_none() {
            request.now_ms = Some(unix_time_ms());
        }
        let deadline = Instant::now() + RENDER_DEADLINE;
        let mut lua = self
            .lua
            .try_borrow_mut()
            .map_err(|_| PixyError::Render("render re-entered the engine".into()))?;
        let result = (|| {
            lua.enter(|ctx| {
                let config = ctx.fetch(&self.config);
                let value = request_value(ctx, &request);
                let function = ctx.fetch(&self.render);
                ctx.fetch(&self.executor)
                    .restart(ctx, function, (config, value));
            });
            self.host.set_context(std::mem::take(&mut request.context));
            self.host.begin_render();
            run(&mut lua, &self.executor, deadline, Some(&self.host)).map_err(render_error)?;
            lua.try_enter(|ctx| {
                let value = ctx.fetch(&self.executor).take_result::<Value>(ctx)??;
                let rewind = match value {
                    Value::Table(table) => table
                        .get::<_, Option<String>>(ctx, "_stream_rewind")?
                        .unwrap_or_default(),
                    _ => String::new(),
                };
                Ok((output_value(ctx, value)?, rewind))
            })
            .map_err(render_error)
        })();
        lua.run_finalizers();
        drop(lua);
        let (output, rewind) = result.map_err(|error| self.render_source_error(error))?;
        validate_output(&output, &request).map_err(|error| self.render_source_error(error))?;
        Ok((output, rewind))
    }

    pub fn list(&self) -> Result<Vec<String>> {
        self.inventory().map(|(selectors, _, _)| selectors)
    }

    pub(crate) fn counts(&self) -> Result<(usize, usize)> {
        self.inventory()
            .map(|(_, zones, segments)| (zones, segments))
    }

    fn inventory(&self) -> Result<(Vec<String>, usize, usize)> {
        let mut lua = self
            .lua
            .try_borrow_mut()
            .map_err(|_| PixyError::Render("inventory re-entered the engine".into()))?;
        lua.try_enter(|ctx| {
            let zones: Table = ctx.fetch(&self.config).get(ctx, "zones")?;
            let mut names = Vec::new();
            let mut zone_count = 0;
            let mut segment_count = 0;
            for (zone_name, zone) in zones.iter(ctx) {
                let zone_name = String::from_value(ctx, zone_name)?;
                let zone = Table::from_value(ctx, zone)?;
                names.push(zone_name.clone());
                zone_count += 1;
                let segments: Table = zone.get(ctx, "segments")?;
                for segment in sequence::<Table>(ctx, segments, MAX_SEGMENTS, "zone segments")? {
                    let segment_name: String = segment.get(ctx, "name")?;
                    names.push(format!("{zone_name}.{segment_name}"));
                    segment_count += 1;
                }
            }
            names.sort();
            Ok((names, zone_count, segment_count))
        })
        .map_err(render_error)
    }

    fn render_source_error(&self, error: PixyError) -> PixyError {
        match error {
            PixyError::Render(message) => {
                PixyError::Render(format!("{}: {message}", self.source_name))
            }
            error => error,
        }
    }
}

/// Drive `executor` to completion one fuel slice at a time, bounding Lua
/// execution by `deadline` and memory by the configured limit. Time the host
/// spent blocked is added back to the deadline.
fn run(
    lua: &mut Lua,
    executor: &StashedExecutor,
    deadline: Instant,
    host: Option<&HostState>,
) -> std::result::Result<(), luna::ExternError> {
    loop {
        let mut fuel = Fuel::with(FUEL_PER_SLICE);
        let finished = lua
            .enter(|ctx| ctx.fetch(executor).step(ctx, &mut fuel))
            .map_err(luna::RuntimeError::new)?;
        if finished {
            return Ok(());
        }
        let allowance = host.map_or(Duration::ZERO, HostState::io_spent);
        if Instant::now() > deadline + allowance {
            lua.enter(|ctx| ctx.fetch(executor).stop(&ctx));
            return Err(luna::RuntimeError::new(DeadlineExceeded).into());
        }
        if let Some(limit) = lua.memory_limit() {
            if lua.total_memory() > limit {
                lua.gc_collect();
                lua.gc_collect();
            }
            if lua.total_memory() > limit {
                lua.enter(|ctx| ctx.fetch(executor).stop(&ctx));
                return Err(luna::RuntimeError::new(MemoryExhausted).into());
            }
        }
    }
}

#[derive(Debug)]
struct DeadlineExceeded;

impl std::fmt::Display for DeadlineExceeded {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str("render deadline exceeded")
    }
}

impl std::error::Error for DeadlineExceeded {}

#[derive(Debug)]
struct MemoryExhausted;

impl std::fmt::Display for MemoryExhausted {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str("memory limit exceeded")
    }
}

impl std::error::Error for MemoryExhausted {}

fn validate_config<'gc>(
    ctx: Context<'gc>,
    config: Table<'gc>,
    source_name: &str,
    deadline: Instant,
) -> Result<()> {
    let zones = config.get::<_, Table>(ctx, "zones").map_err(|error| {
        PixyError::Config(format!(
            "{source_name}: config zones must be a table: {error}"
        ))
    })?;
    for (name, zone) in zones.iter(ctx) {
        if Instant::now() > deadline {
            return Err(PixyError::Config(format!(
                "{source_name}: config validation deadline exceeded"
            )));
        }
        let Value::String(name) = name else {
            return Err(PixyError::Config(format!(
                "{source_name}: zone names must be strings"
            )));
        };
        let name = name.to_str().map_err(|error| {
            PixyError::Config(format!("{source_name}: zone name is not UTF-8: {error}"))
        })?;
        if !crate::model::output::valid_selector(name) {
            return Err(PixyError::Config(format!(
                "{source_name}: invalid zone name {name:?}"
            )));
        }
        let Value::Table(zone) = zone else {
            return Err(PixyError::Config(format!(
                "{source_name}: zone {name} is not a pixy.zone"
            )));
        };
        if zone.get::<_, String>(ctx, "kind").ok().as_deref() != Some("pixy_zone") {
            return Err(PixyError::Config(format!(
                "{source_name}: zone {name} is not a pixy.zone"
            )));
        }
        validate_zone(ctx, zone, name, source_name, deadline)?;
    }
    Ok(())
}

fn validate_zone<'gc>(
    ctx: Context<'gc>,
    zone: Table<'gc>,
    zone_name: &str,
    source_name: &str,
    deadline: Instant,
) -> Result<()> {
    let segments = zone.get::<_, Table>(ctx, "segments").map_err(|error| {
        PixyError::Config(format!(
            "{source_name}: zone {zone_name} segments must be a list: {error}"
        ))
    })?;
    let mut names = BTreeSet::new();
    let mut count = 0_usize;
    let mut highest = 0_usize;
    let segment_index = Table::new(&ctx);
    for (index, segment) in segments.iter(ctx) {
        if Instant::now() > deadline {
            return Err(PixyError::Config(format!(
                "{source_name}: config validation deadline exceeded"
            )));
        }
        let Value::Integer(index) = index else {
            return Err(PixyError::Config(format!(
                "{source_name}: zone {zone_name} segments must be an array"
            )));
        };
        let Ok(index) = usize::try_from(index) else {
            return Err(PixyError::Config(format!(
                "{source_name}: zone {zone_name} has an invalid segment index"
            )));
        };
        if index == 0 {
            return Err(PixyError::Config(format!(
                "{source_name}: zone {zone_name} has an invalid segment index"
            )));
        }
        count += 1;
        highest = highest.max(index);
        let Value::Table(segment) = segment else {
            return Err(PixyError::Config(format!(
                "{source_name}: zone {zone_name} contains a non-segment value"
            )));
        };
        if segment.get::<_, String>(ctx, "kind").ok().as_deref() != Some("pixy_segment") {
            return Err(PixyError::Config(format!(
                "{source_name}: zone {zone_name} contains a non-pixy.segment value"
            )));
        }
        let segment_name = segment.get::<_, String>(ctx, "name").map_err(|error| {
            PixyError::Config(format!(
                "{source_name}: zone {zone_name} segment name is invalid: {error}"
            ))
        })?;
        if !valid_segment_name(&segment_name) {
            return Err(PixyError::Config(format!(
                "{source_name}: invalid segment name {zone_name}.{segment_name}"
            )));
        }
        if !names.insert(segment_name.clone()) {
            return Err(PixyError::Config(format!(
                "{source_name}: duplicate segment {zone_name}.{segment_name}"
            )));
        }
        if !matches!(segment.get_value(ctx, "render"), Value::Function(_)) {
            return Err(PixyError::Config(format!(
                "{source_name}: segment {zone_name}.{segment_name} render value is not a function"
            )));
        }
        if !matches!(segment.get_value(ctx, "options"), Value::Table(_)) {
            return Err(PixyError::Config(format!(
                "{source_name}: segment {zone_name}.{segment_name} options must be a table"
            )));
        }
        segment_index
            .set(ctx, segment_name, segment)
            .map_err(|error| {
                PixyError::Config(format!("{source_name}: zone {zone_name}: {error}"))
            })?;
    }
    if count == 0 || count != highest {
        return Err(PixyError::Config(format!(
            "{source_name}: zone {zone_name} requires a dense, non-empty segment list"
        )));
    }
    zone.set_field(ctx, "segment_index", segment_index);
    Ok(())
}

fn valid_segment_name(name: &str) -> bool {
    let mut chars = name.chars();
    matches!(chars.next(), Some(first) if first.is_ascii_alphanumeric())
        && chars.all(|ch| ch.is_ascii_alphanumeric() || matches!(ch, '_' | '-'))
}

fn install_modules(lua: &mut Lua, deadline: Instant, host: &HostState) -> Result<()> {
    lua.try_enter(|ctx| {
        ctx.set_global("dofile", Value::Nil);
        ctx.set_global("loadfile", Value::Nil);
        let package: Table = ctx.get_global("package")?;
        let preload: Table = package.get(ctx, "preload")?;
        for &(name, source) in MODULES {
            let chunk = format!("@bundled/{name}.lua");
            let closure = Closure::load(ctx, Some(&chunk), source.as_bytes())?;
            preload.set(ctx, name, closure)?;
        }
        Ok(())
    })
    .map_err(render_error)?;
    let loader = lua
        .try_enter(|ctx| {
            let closure = Closure::load(
                ctx,
                Some("@pixy/module-loader.lua"),
                MODULE_LOADER.as_bytes(),
            )?;
            Ok(ctx.stash(Executor::start(ctx, closure.into(), ())))
        })
        .map_err(render_error)?;
    run(lua, &loader, deadline, Some(host)).map_err(render_error)
}

fn request_value<'gc>(ctx: Context<'gc>, request: &RenderRequest) -> Value<'gc> {
    let table = Table::new(&ctx);
    table.set_field(ctx, "version", request.version);
    table.set_field(ctx, "select", request.select.clone());
    table.set_field(ctx, "mode", mode_name(request.mode));
    table.set_field(ctx, "target", request.target.map(target_name));
    table.set_field(ctx, "width", request.width);
    table.set_field(ctx, "height", request.height);
    table.set_field(ctx, "now_ms", request.now_ms);
    table.set_field(ctx, "context", context_value(ctx, &request.context));
    table.set_field(ctx, "ignore_missing", request.ignore_missing);
    table.into()
}

fn context_value<'gc>(ctx: Context<'gc>, context: &RenderContext) -> Value<'gc> {
    let table = Table::new(&ctx);
    let env = Table::new(&ctx);
    for (name, value) in &context.env {
        let _ = env.set(ctx, name.as_str(), value.clone());
    }
    table.set_field(ctx, "env", env);
    let values = Table::new(&ctx);
    for (name, value) in &context.values {
        let _ = values.set(ctx, name.as_str(), json_value(ctx, value));
    }
    table.set_field(ctx, "values", values);
    table.into()
}

fn json_value<'gc>(ctx: Context<'gc>, value: &serde_json::Value) -> Value<'gc> {
    match value {
        serde_json::Value::Null => Value::Nil,
        serde_json::Value::Bool(value) => Value::Boolean(*value),
        serde_json::Value::Number(number) => number
            .as_i64()
            .map(Value::Integer)
            .unwrap_or_else(|| Value::Number(number.as_f64().unwrap_or_default())),
        serde_json::Value::String(value) => Value::String(ctx.intern(value.as_bytes())),
        serde_json::Value::Array(items) => {
            let table = Table::new(&ctx);
            for (index, item) in items.iter().enumerate() {
                let _ = table.set(ctx, index as i64 + 1, json_value(ctx, item));
            }
            table.into()
        }
        serde_json::Value::Object(entries) => {
            let table = Table::new(&ctx);
            for (name, item) in entries {
                let _ = table.set(ctx, name.as_str(), json_value(ctx, item));
            }
            table.into()
        }
    }
}

fn mode_name(mode: crate::model::output::RenderMode) -> &'static str {
    match mode {
        crate::model::output::RenderMode::Line => "line",
        crate::model::output::RenderMode::Run => "run",
        crate::model::output::RenderMode::Surface => "surface",
    }
}

fn target_name(target: crate::model::output::LineTarget) -> &'static str {
    match target {
        crate::model::output::LineTarget::Plain => "plain",
        crate::model::output::LineTarget::Ansi => "ansi",
        crate::model::output::LineTarget::Bash => "bash",
        crate::model::output::LineTarget::Zsh => "zsh",
    }
}

fn output_value<'gc>(ctx: Context<'gc>, value: Value<'gc>) -> luna::Result<'gc, RenderOutput> {
    let Value::Table(table) = value else {
        return Err(type_error(ctx, "render output is not a table"));
    };
    let next_frame_ms = table.get::<_, Option<u64>>(ctx, "next_frame_ms")?;
    let regions = regions_value(ctx, table.get_value(ctx, "regions"))?;
    match table.get::<_, String>(ctx, "mode")?.as_str() {
        "line" => Ok(RenderOutput::Line {
            text: table.get(ctx, "text")?,
            width: table.get(ctx, "width")?,
            next_frame_ms,
            regions,
        }),
        "run" => Ok(RenderOutput::Run {
            runs: runs_value(ctx, table.get_value(ctx, "runs"))?,
            width: table.get(ctx, "width")?,
            next_frame_ms,
            regions,
        }),
        "surface" => Ok(RenderOutput::Surface {
            ansi: table.get(ctx, "ansi")?,
            width: table.get(ctx, "width")?,
            height: table.get(ctx, "height")?,
            next_frame_ms,
            regions,
        }),
        mode => Err(type_error(ctx, format!("unknown mode {mode:?}"))),
    }
}

/// Walk a Lua array like `ipairs`: stop at the first `nil`, reject past `limit`.
/// `#t` is unspecified for a table with holes and never bounds this loop.
fn sequence<'gc, V: luna::FromValue<'gc>>(
    ctx: Context<'gc>,
    table: Table<'gc>,
    limit: usize,
    what: &'static str,
) -> luna::Result<'gc, Vec<V>> {
    let mut items = Vec::new();
    for index in 1_i64.. {
        let Some(item) = table.get::<_, Option<V>>(ctx, index)? else {
            break;
        };
        if items.len() >= limit {
            return Err(type_error(ctx, format!("{what} exceeds {limit} entries")));
        }
        items.push(item);
    }
    Ok(items)
}

fn runs_value<'gc>(ctx: Context<'gc>, value: Value<'gc>) -> luna::Result<'gc, Vec<Run>> {
    let Value::Table(table) = value else {
        return Err(type_error(ctx, "run output is not a list"));
    };
    let mut runs = Vec::new();
    for run in sequence::<Table>(ctx, table, MAX_RUNS, "run output")? {
        runs.push(Run {
            text: run.get(ctx, "text")?,
            style: run
                .get::<_, Option<String>>(ctx, "style")?
                .unwrap_or_default(),
        });
    }
    Ok(runs)
}

fn regions_value<'gc>(
    ctx: Context<'gc>,
    value: Value<'gc>,
) -> luna::Result<'gc, Vec<crate::model::output::OutputRegion>> {
    let Value::Table(table) = value else {
        return Ok(Vec::new());
    };
    let mut regions = Vec::new();
    for region in sequence::<Table>(ctx, table, MAX_REGIONS, "interactive regions")? {
        regions.push(crate::model::output::OutputRegion {
            id: region.get(ctx, "id")?,
            x: region.get(ctx, "x")?,
            y: region.get(ctx, "y")?,
            width: region.get(ctx, "width")?,
            height: region.get(ctx, "height")?,
            actions: region
                .get::<_, Option<_>>(ctx, "actions")?
                .unwrap_or_default(),
            hover_style: region
                .get::<_, Option<String>>(ctx, "hover_style")?
                .unwrap_or_default(),
            press_styles: region
                .get::<_, Option<_>>(ctx, "press_styles")?
                .unwrap_or_default(),
        });
    }
    Ok(regions)
}

fn type_error<'gc>(ctx: Context<'gc>, message: impl std::fmt::Display) -> luna::Error<'gc> {
    Value::String(ctx.intern(message.to_string().as_bytes())).into()
}
fn validate_output(output: &RenderOutput, request: &RenderRequest) -> Result<()> {
    match output {
        RenderOutput::Line { text, width, .. } => {
            if request.mode != crate::model::output::RenderMode::Line {
                return Err(PixyError::Render(
                    "zone selection returned the wrong output mode".into(),
                ));
            }
            if *width > usize::from(request.width) {
                return Err(PixyError::Render(
                    "render width exceeds requested geometry".into(),
                ));
            }
            if text.len() > OUTPUT_LIMIT {
                return Err(PixyError::Render("line output exceeds 1 MiB".into()));
            }
            if text.contains(['\n', '\r']) {
                return Err(PixyError::Render("line output contains a newline".into()));
            }
        }
        RenderOutput::Run { runs, width, .. } => {
            if request.mode != crate::model::output::RenderMode::Run {
                return Err(PixyError::Render(
                    "zone selection returned the wrong output mode".into(),
                ));
            }
            if *width > usize::from(request.width) {
                return Err(PixyError::Render(
                    "render width exceeds requested geometry".into(),
                ));
            }
            let mut size = 0;
            for run in runs {
                size += run.text.len() + run.style.len();
                if run.text.chars().any(|ch| ch.is_control()) {
                    return Err(PixyError::Render(
                        "run output contains a control byte".into(),
                    ));
                }
            }
            if size > OUTPUT_LIMIT {
                return Err(PixyError::Render("run output exceeds 1 MiB".into()));
            }
        }
        RenderOutput::Surface {
            ansi,
            width,
            height,
            ..
        } => {
            if request.mode != crate::model::output::RenderMode::Surface {
                return Err(PixyError::Render(
                    "zone selection returned the wrong output mode".into(),
                ));
            }
            if *width > request.width || *height > request.height {
                return Err(PixyError::Render(
                    "surface exceeds requested geometry".into(),
                ));
            }
            if ansi.len() > OUTPUT_LIMIT {
                return Err(PixyError::Render("surface output exceeds 1 MiB".into()));
            }
        }
    }
    let encoded = serde_json::to_vec(output)
        .map_err(|error| PixyError::Render(format!("failed to size output: {error}")))?;
    if encoded.len() > OUTPUT_LIMIT {
        return Err(PixyError::Render(
            "serialized output exceeds 1 MiB limit".into(),
        ));
    }
    validate_regions(output, request)?;
    Ok(())
}

fn validate_regions(output: &RenderOutput, request: &RenderRequest) -> Result<()> {
    let regions = output.regions();
    if regions.len() > 4_096 {
        return Err(PixyError::Render(
            "output has too many interactive regions".into(),
        ));
    }
    let height = if request.mode == crate::model::output::RenderMode::Surface {
        request.height
    } else {
        1
    };
    for region in regions {
        let right = region.x.checked_add(region.width);
        let bottom = region.y.checked_add(region.height);
        if !crate::model::output::valid_selector(&region.id)
            || region.width == 0
            || region.height == 0
            || right.is_none_or(|value| value > request.width)
            || bottom.is_none_or(|value| value > height)
        {
            return Err(PixyError::Render(format!(
                "invalid interactive region {:?}",
                region.id
            )));
        }
        for (event, action) in &region.actions {
            if !crate::model::output::valid_selector(event)
                || !crate::model::output::valid_selector(action)
            {
                return Err(PixyError::Render(format!(
                    "invalid action on interactive region {:?}",
                    region.id
                )));
            }
        }
        for button in region.press_styles.keys() {
            if !crate::model::output::valid_selector(button) {
                return Err(PixyError::Render(format!(
                    "invalid press style on interactive region {:?}",
                    region.id
                )));
            }
        }
    }
    Ok(())
}

fn unix_time_ms() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}

fn render_error(error: luna::ExternError) -> PixyError {
    PixyError::Render(error.to_string())
}

#[cfg(test)]
mod tests {
    use super::{Engine, validate_output};
    use crate::model::output::{LineTarget, RenderMode, RenderOutput, RenderRequest, Run};

    #[test]
    fn renders_bundled_view() {
        let root = std::env::temp_dir();
        let paths = crate::runtime::config::Paths {
            config_dir: root.clone(),
            cache_dir: root.clone(),
            data_dir: root.clone(),
        };
        let engine = Engine::load(
            crate::runtime::config::ConfigSource {
                name: "@pixy/default.lua".into(),
                source: crate::runtime::config::DEFAULT_CONFIG.into(),
                directory: root,
                path: None,
            },
            &paths,
        )
        .expect("engine");
        let output = engine
            .render(RenderRequest {
                select: vec!["demo.pixy1".into()],
                mode: RenderMode::Line,
                target: Some(LineTarget::Plain),
                ..RenderRequest::default()
            })
            .expect("render");
        assert!(
            matches!(output, crate::model::output::RenderOutput::Line { text, .. } if text.contains("pixy"))
        );
    }

    #[test]
    fn exact_serialized_output_size_is_bounded() {
        let output = RenderOutput::Run {
            runs: (0..50_000)
                .map(|index| Run {
                    text: "x".into(),
                    style: format!("fg:{}", index % 2),
                })
                .collect(),
            width: 50_000,
            next_frame_ms: None,
            regions: Vec::new(),
        };
        let request = RenderRequest {
            mode: RenderMode::Run,
            target: None,
            width: u16::MAX,
            ..RenderRequest::default()
        };
        assert!(validate_output(&output, &request).is_err());
    }
}
