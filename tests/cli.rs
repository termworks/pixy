use std::io::Write;
use std::process::{Command, Stdio};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

fn pixy(args: &[&str]) -> std::process::Output {
    Command::new(env!("CARGO_BIN_EXE_pixy"))
        .args(args)
        .env_remove("PIXY_CONFIG")
        .env(
            "XDG_CONFIG_HOME",
            std::env::temp_dir().join(format!("pixy-cli-config-{}", std::process::id())),
        )
        .stdin(Stdio::null())
        .output()
        .expect("run pixy")
}

fn pixy_input(args: &[&str], input: &str) -> std::process::Output {
    let mut child = Command::new(env!("CARGO_BIN_EXE_pixy"))
        .args(args)
        .env_remove("PIXY_CONFIG")
        .env(
            "XDG_CONFIG_HOME",
            std::env::temp_dir().join(format!("pixy-cli-config-{}", std::process::id())),
        )
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect("run pixy");
    child
        .stdin
        .take()
        .expect("stdin")
        .write_all(input.as_bytes())
        .expect("request");
    child.wait_with_output().expect("output")
}

#[test]
fn render_and_shorthand_match_without_newline() {
    let render = pixy(&["render", "demo", "--target", "plain", "--set", "status=7"]);
    let shorthand = pixy(&["demo", "--target", "plain", "--set", "status=7"]);
    assert!(
        render.status.success(),
        "{}",
        String::from_utf8_lossy(&render.stderr)
    );
    assert_eq!(render.stdout, shorthand.stdout);
    assert!(!render.stdout.ends_with(b"\n"));
}

#[test]
fn diagnostics_use_stderr_and_usage_exit_code() {
    let output = pixy(&["render", "missing", "--target", "plain"]);
    assert_eq!(output.status.code(), Some(4));
    assert!(output.stdout.is_empty());
    assert!(!output.stderr.is_empty());
    let usage = pixy(&["render", "bad/name"]);
    assert_eq!(usage.status.code(), Some(2));
    assert_eq!(
        pixy(&["stream", "activity.spinner", "--fps", "0"])
            .status
            .code(),
        Some(2)
    );
    assert_eq!(
        pixy(&["stream", "activity.spinner", "--duration", "86400001"])
            .status
            .code(),
        Some(2)
    );
}

#[test]
fn lists_and_checks_bundled_config() {
    let list = pixy(&["list"]);
    assert!(list.status.success());
    let list = String::from_utf8(list.stdout).expect("list output");
    assert!(list.lines().any(|line| line == "demo"));
    assert!(list.lines().any(|line| line == "demo.pixy1"));
    assert!(list.lines().any(|line| line == "prompt.left.directory"));
    let check = pixy(&["check"]);
    assert!(check.status.success());
    assert!(
        String::from_utf8(check.stdout)
            .expect("check output")
            .contains("zones")
    );
}

#[test]
fn inherited_columns_sets_default_render_width() {
    let output = Command::new(env!("CARGO_BIN_EXE_pixy"))
        .args([
            "render",
            "demo.pixy1,demo.pixy1,demo.pixy1,demo.pixy1",
            "--target",
            "plain",
        ])
        .env_remove("PIXY_CONFIG")
        .env("COLUMNS", "17")
        .env(
            "XDG_CONFIG_HOME",
            std::env::temp_dir().join(format!("pixy-cli-columns-{}", std::process::id())),
        )
        .output()
        .expect("run pixy");
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    assert_eq!(output.stdout, b" pixy  pixy ");
}

#[test]
fn request_and_context_json_precedence_is_explicit() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .expect("time")
        .as_nanos();
    let root = std::env::temp_dir().join(format!("pixy-cli-{}-{unique}", std::process::id()));
    std::fs::create_dir_all(&root).expect("root");
    let config = root.join("init.lua");
    std::fs::write(
        &config,
        "local pixy=require('pixy'); return pixy.config({zones={x=pixy.zone({pixy.segment('value',function(ctx) return tostring(ctx.values.status)..':'..tostring(ctx.values.key) end)})}})",
    )
    .expect("config");
    let output = pixy(&[
        "render",
        "x",
        "--target",
        "plain",
        "--set",
        "status=1",
        "--set",
        "key=flag",
        "--context-json",
        "{\"values\":{\"status\":2,\"key\":\"json\"}}",
        "--config",
        config.to_str().expect("config"),
        "--newline",
    ]);
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    assert_eq!(output.stdout, b"2:json\n");

    let request = r#"{"version":1,"select":["demo.pixy1"],"mode":"line","target":"plain","width":80,"height":1,"now_ms":0,"context":{}}"#;
    let output = pixy_input(
        &[
            "render",
            "demo.pixy2",
            "--target",
            "ansi",
            "--set",
            "status=9",
            "--request",
            "-",
        ],
        request,
    );
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    assert_eq!(output.stdout, b" pixy ");
    std::fs::remove_dir_all(root).expect("cleanup");
}

#[test]
fn config_and_request_failures_use_documented_exit_codes() {
    let config = pixy(&["check", "--config", "/definitely/missing/pixy.lua"]);
    assert_eq!(config.status.code(), Some(3));
    let request = pixy(&["render", "--request", "/definitely/missing/request.json"]);
    assert_eq!(request.status.code(), Some(2));
    let invalid = pixy_input(&["render", "--request", "-"], "{}");
    assert_eq!(invalid.status.code(), Some(2));
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .expect("time")
        .as_nanos();
    let root = std::env::temp_dir().join(format!("pixy-invalid-{}-{unique}", std::process::id()));
    std::fs::create_dir_all(&root).expect("root");
    let invalid_config = root.join("invalid.lua");
    std::fs::write(&invalid_config, "return {}").expect("invalid config");
    let output = pixy(&[
        "check",
        "--config",
        invalid_config.to_str().expect("config"),
    ]);
    assert_eq!(output.status.code(), Some(3));
    std::fs::remove_dir_all(root).expect("cleanup");
}

#[test]
fn stream_duration_caps_distant_animation_deadlines() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .expect("time")
        .as_nanos();
    let root = std::env::temp_dir().join(format!("pixy-stream-{}-{unique}", std::process::id()));
    std::fs::create_dir_all(&root).expect("root");
    let config = root.join("init.lua");
    std::fs::write(
        &config,
        "local pixy=require('pixy'); return pixy.config({zones={x=pixy.zone({pixy.segment('animated',function(ctx) return pixy.animate({interval_ms=60000,started_at_ms=ctx.now_ms,callback=function() return 'x' end}) end)})}})",
    )
    .expect("config");
    let started = Instant::now();
    let output = pixy(&[
        "stream",
        "x",
        "--target",
        "plain",
        "--duration=20",
        "--fps=120",
        "--config",
        config.to_str().expect("config"),
    ]);
    assert!(output.status.success());
    assert_eq!(output.stdout, b"x");
    assert!(started.elapsed() < Duration::from_millis(500));
    std::fs::remove_dir_all(root).expect("cleanup");
}
