use pixy::config::{ConfigSource, Paths};
use pixy::{Engine, LineTarget, RenderMode, RenderOutput, RenderRequest};
use std::time::{Duration, Instant};

fn engine(body: &str) -> Engine {
    let root = std::env::temp_dir();
    let paths = Paths {
        config_dir: root.clone(),
        cache_dir: root.clone(),
        data_dir: root.clone(),
    };
    let source = ConfigSource {
        name: "@test.lua".into(),
        source: body.into(),
        directory: root,
        path: None,
    };
    Engine::load(source, &paths).expect("load engine")
}

fn load(body: &str) -> pixy::Result<Engine> {
    let root = std::env::temp_dir();
    let paths = Paths {
        config_dir: root.clone(),
        cache_dir: root.clone(),
        data_dir: root.clone(),
    };
    let source = ConfigSource {
        name: "@test.lua".into(),
        source: body.into(),
        directory: root,
        path: None,
    };
    Engine::load(source, &paths)
}

fn request(select: &str, target: LineTarget) -> RenderRequest {
    RenderRequest {
        select: select.split(',').map(str::to_string).collect(),
        target: Some(target),
        now_ms: Some(160),
        ..RenderRequest::default()
    }
}

#[test]
fn renders_whole_zones_and_named_segments() {
    let engine = engine(
        r#"
local pixy = require("pixy")
return pixy.config({zones = {
  a = pixy.zone({pixy.segment("value", function() return "a" end)}),
  b = pixy.zone({pixy.segment("value", function() return "b" end)}),
}})
"#,
    );
    let output = engine
        .render(request("b,a,b", LineTarget::Plain))
        .expect("render");
    assert!(matches!(output, RenderOutput::Line { text, width: 3, .. } if text == "bab"));
    let output = engine
        .render(request("a.value", LineTarget::Plain))
        .expect("render segment");
    assert!(matches!(output, RenderOutput::Line { text, width: 1, .. } if text == "a"));
    assert_eq!(
        engine.list().expect("zone inventory"),
        ["a", "a.value", "b", "b.value"]
    );
}

#[test]
fn handles_missing_selectors_explicitly() {
    let engine = engine(
        "local pixy=require('pixy'); return pixy.config({zones={a=pixy.zone({pixy.segment('value',function() return 'a' end)})}})",
    );
    assert!(
        engine
            .render(request("missing", LineTarget::Plain))
            .is_err()
    );
    let mut ignored = request("missing,a", LineTarget::Plain);
    ignored.ignore_missing = true;
    assert!(matches!(engine.render(ignored), Ok(RenderOutput::Line { text, .. }) if text == "a"));
}

#[test]
fn invalid_nodes_name_the_zone_selection() {
    let engine = engine(
        "local pixy=require('pixy'); return pixy.config({zones={broken=pixy.zone({pixy.segment('value',function() return {kind='unknown'} end)})}})",
    );
    let error = engine
        .render(request("broken", LineTarget::Plain))
        .expect_err("invalid node");
    assert!(error.to_string().contains("zone selection broken"));
    assert!(error.to_string().contains("@test.lua"));
}

#[test]
fn enforces_render_deadline() {
    let engine = engine(
        "local pixy=require('pixy'); return pixy.config({zones={loop=pixy.zone({pixy.segment('value',function() while true do end end)})}})",
    );
    let started = Instant::now();
    assert!(engine.render(request("loop", LineTarget::Plain)).is_err());
    assert!(started.elapsed() < Duration::from_secs(1));
}

#[test]
fn enforces_config_load_deadline() {
    let started = Instant::now();
    assert!(load("while true do end").is_err());
    assert!(started.elapsed() < Duration::from_secs(1));
}

#[test]
fn reports_config_syntax_errors() {
    let error = match load("return {") {
        Ok(_) => panic!("invalid config loaded"),
        Err(error) => error,
    };
    assert!(error.to_string().contains("test.lua"));
}

#[test]
fn rejects_invalid_config_shapes_and_zone_registries() {
    for source in [
        "return {}",
        "return {zones={x='not a zone'}}",
        "return {zones={['bad/name']={kind='pixy_zone',segments={}}}}",
        "return {zones={x={kind='pixy_zone',segments={}}}}",
    ] {
        assert!(matches!(load(source), Err(pixy::PixyError::Config(_))));
    }
}

#[test]
fn enforces_memory_limit() {
    let engine = engine(
        "local pixy=require('pixy'); return pixy.config({zones={large=pixy.zone({pixy.segment('value',function() return string.rep('x', 40 * 1024 * 1024) end)})}})",
    );
    assert!(engine.render(request("large", LineTarget::Plain)).is_err());
}

#[test]
fn rejects_oversized_config_source_before_evaluation() {
    assert!(matches!(
        load(&format!("--{}", "x".repeat(1024 * 1024))),
        Err(pixy::PixyError::Config(_))
    ));
}

#[test]
fn measures_unicode_cells() {
    let engine = engine(
        "local pixy=require('pixy'); return pixy.config({zones={wide=pixy.zone({pixy.segment('value',function() return '界é' end)})}})",
    );
    let output = engine
        .render(request("wide", LineTarget::Plain))
        .expect("render");
    assert!(matches!(output, RenderOutput::Line { width: 3, .. }));
}

#[test]
fn renders_run_and_surface_modes() {
    let engine = engine(
        "local pixy=require('pixy'); return pixy.config({zones={x=pixy.zone({pixy.segment('value',function() return pixy.column({pixy.text('a',{fg=1}), 'b'}) end)})}})",
    );
    let mut surface = request("x", LineTarget::Ansi);
    surface.mode = RenderMode::Surface;
    surface.width = 2;
    surface.height = 1;
    assert!(matches!(
        engine.render(surface),
        Ok(RenderOutput::Surface {
            width: 1,
            height: 1,
            ..
        })
    ));
}

#[test]
fn priority_removal_keeps_declaration_order() {
    let engine = engine(
        "local pixy=require('pixy'); return pixy.config({zones={x=pixy.zone({pixy.segment('a',function() return 'aa' end,{priority=0}),pixy.segment('b',function() return 'bb' end,{priority=0})})}})",
    );
    let mut request = request("x", LineTarget::Plain);
    request.width = 2;
    assert!(matches!(engine.render(request), Ok(RenderOutput::Line { text, .. }) if text == "aa"));
}

#[test]
fn priority_removal_retains_lower_numbers() {
    let engine = engine(
        "local pixy=require('pixy'); return pixy.config({zones={x=pixy.zone({pixy.segment('a',function() return 'aa' end,{priority=1}),pixy.segment('b',function() return 'bb' end,{priority=100})})}})",
    );
    let mut request = request("x", LineTarget::Plain);
    request.width = 2;
    assert!(matches!(engine.render(request), Ok(RenderOutput::Line { text, .. }) if text == "aa"));
}

#[test]
fn rejects_file_loader_globals() {
    let engine = engine(
        "local pixy=require('pixy'); return pixy.config({zones={safe=pixy.zone({pixy.segment('value',function() return tostring(io == nil and os == nil and debug == nil and dofile == nil and loadfile == nil and package.loadlib == nil and package.path == '' and #package.searchers == 2) end)})}})",
    );
    assert!(
        matches!(engine.render(request("safe", LineTarget::Plain)), Ok(RenderOutput::Line { text, .. }) if text == "true")
    );
}

#[test]
fn config_cannot_replace_the_engine_render_entrypoint() {
    let engine = engine(
        "local pixy=require('pixy'); require=function() while true do end end; return pixy.config({zones={x=pixy.zone({pixy.segment('value',function() return 'x' end)})}})",
    );
    assert!(
        matches!(engine.render(request("x", LineTarget::Plain)), Ok(RenderOutput::Line { text, .. }) if text == "x")
    );
}

#[test]
fn loads_only_bundled_or_trusted_config_modules() {
    let unique = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .expect("time")
        .as_nanos();
    let root = std::env::temp_dir().join(format!("pixy-modules-{}-{unique}", std::process::id()));
    let config_dir = root.join("config");
    std::fs::create_dir_all(config_dir.join("lua/nested")).expect("module directories");
    std::fs::write(config_dir.join("localmod.lua"), "return 'local'").expect("local module");
    std::fs::write(config_dir.join("lua/nested/init.lua"), "return 'nested'")
        .expect("nested module");
    std::fs::write(root.join("outside.lua"), "return 'outside'").expect("outside module");
    std::os::unix::fs::symlink(root.join("outside.lua"), config_dir.join("escape.lua"))
        .expect("escape module");
    let paths = Paths {
        config_dir: config_dir.clone(),
        cache_dir: root.join("cache"),
        data_dir: root.join("data"),
    };
    let source = ConfigSource {
        name: "@module-test.lua".into(),
        source: "local pixy=require('pixy'); local a=require('localmod'); local b=require('nested'); return pixy.config({zones={x=pixy.zone({pixy.segment('value',function() return a..':'..b end)})}})".into(),
        directory: config_dir.clone(),
        path: None,
    };
    let engine = Engine::load(source, &paths).expect("trusted modules");
    assert!(
        matches!(engine.render(request("x", LineTarget::Plain)), Ok(RenderOutput::Line { text, .. }) if text == "local:nested")
    );
    let escaped = ConfigSource {
        name: "@escape-test.lua".into(),
        source: "require('escape'); return {zones={}}".into(),
        directory: config_dir,
        path: None,
    };
    assert!(matches!(
        Engine::load(escaped, &paths),
        Err(pixy::PixyError::Config(_))
    ));
    std::fs::remove_dir_all(root).expect("cleanup");
}
