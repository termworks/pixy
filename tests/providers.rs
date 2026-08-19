use pixy::config::{ConfigSource, Paths};
use pixy::context::RenderContext;
use pixy::{Engine, LineTarget, RenderOutput, RenderRequest};
use serde_json::json;
use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

fn root() -> PathBuf {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .expect("time")
        .as_nanos();
    let root = std::env::temp_dir().join(format!("pixy-provider-{}-{unique}", std::process::id()));
    fs::create_dir_all(&root).expect("root");
    root
}

fn engine(root: &Path, source: String) -> Engine {
    let paths = Paths {
        config_dir: root.into(),
        cache_dir: root.into(),
        data_dir: root.into(),
    };
    Engine::load(
        ConfigSource {
            name: "@providers.lua".into(),
            source,
            directory: root.into(),
            path: None,
        },
        &paths,
    )
    .expect("engine")
}

fn text(engine: &Engine, name: &str, values: &[(&str, serde_json::Value)]) -> String {
    let mut request = RenderRequest {
        select: vec![name.into()],
        target: Some(LineTarget::Plain),
        now_ms: Some(2_000),
        ..RenderRequest::default()
    };
    for (key, value) in values {
        request.context.values.insert((*key).into(), value.clone());
    }
    match engine.render(request).expect("render") {
        RenderOutput::Line { text, .. } => text,
        _ => panic!("unexpected output"),
    }
}

fn text_with(engine: &Engine, name: &str, context: RenderContext, now_ms: u64) -> String {
    let request = RenderRequest {
        select: vec![name.into()],
        target: Some(LineTarget::Plain),
        now_ms: Some(now_ms),
        context,
        ..RenderRequest::default()
    };
    match engine.render(request).expect("render") {
        RenderOutput::Line { text, .. } => text,
        _ => panic!("unexpected output"),
    }
}

#[test]
fn system_and_git_providers_parse_fixtures() {
    let root = root();
    fs::write(root.join("uptime"), "123.50 20.0\n").expect("uptime");
    fs::write(
        root.join("meminfo"),
        "MemTotal: 1000 kB\nMemAvailable: 250 kB\n",
    )
    .expect("memory");
    fs::write(root.join("stat"), "cpu  1 2 3 4 5 6 7 8\n").expect("stat");
    fs::write(
        root.join("netdev"),
        "Inter-| Receive | Transmit\n eth0: 10 0 0 0 0 0 0 0 20 0\n",
    )
    .expect("network");
    let source = r#"
local pixy=require("pixy")
local system=require("pixy.segments.system")
local git=require("pixy.segments.git")
local function zone(renderer) return pixy.zone({pixy.segment("value",renderer)}) end
return pixy.config({zones={
  uptime=zone(function(ctx) return tostring(system.uptime(ctx)) end),
  memory=zone(function(ctx) local x=system.memory(ctx); return x and tostring(x.used_kib) or "nil" end),
  cpu=zone(function(ctx) local x=system.cpu(ctx); return x and tostring(x.total) or "nil" end),
  network=zone(function(ctx) local x=system.network(ctx); return x and tostring(x.received)..":"..tostring(x.sent) or "nil" end),
  git=zone(function(ctx) return tostring(git.branch(ctx))..":"..tostring(git.status(ctx)) end),
}})
"#;
    let engine = engine(&root, source.into());
    assert_eq!(
        text(&engine, "uptime", &[("uptime_path", json!("uptime"))]),
        "123.5"
    );
    assert_eq!(
        text(&engine, "memory", &[("meminfo_path", json!("meminfo"))]),
        "750"
    );
    assert_eq!(text(&engine, "cpu", &[("stat_path", json!("stat"))]), "36");
    assert_eq!(
        text(&engine, "network", &[("netdev_path", json!("netdev"))]),
        "10:20"
    );
    assert_eq!(
        text(
            &engine,
            "git",
            &[
                ("git_branch", json!("main")),
                ("git_status", json!("dirty"))
            ]
        ),
        "main:dirty"
    );
    fs::remove_dir_all(root).expect("cleanup");
}

#[test]
fn process_host_is_timed_capped_and_cached() {
    let root = root();
    let counter = root.join("count");
    let script = root.join("counter.sh");
    let env_script = root.join("environment.sh");
    let descendant_script = root.join("descendant.sh");
    fs::write(&script, "#!/bin/sh\nprintf 'x' >> \"$1\"\nprintf cached\n").expect("script");
    fs::set_permissions(&script, fs::Permissions::from_mode(0o755)).expect("mode");
    fs::write(&env_script, "#!/bin/sh\nprintf '%s' \"$TOKEN\"\n").expect("environment script");
    fs::set_permissions(&env_script, fs::Permissions::from_mode(0o755)).expect("mode");
    fs::write(&descendant_script, "#!/bin/sh\nsleep 5 &\nprintf done\n")
        .expect("descendant script");
    fs::set_permissions(&descendant_script, fs::Permissions::from_mode(0o755)).expect("mode");
    let source = format!(
        r#"
local pixy=require("pixy")
local function zone(renderer) return pixy.zone({{pixy.segment("value",renderer)}}) end
return pixy.config({{zones={{
  timeout=zone(function() local x=pixy.host.exec({{"sleep","1"}},{{timeout_ms=10}}); return tostring(x.timed_out) end),
  cap=zone(function() local x=pixy.host.exec({{"yes"}},{{timeout_ms=10}}); return tostring(x.truncated) end),
  cache=zone(function() local a=pixy.host.exec({{{:?},{:?}}},{{timeout_ms=100,ttl_ms=1000}}); local b=pixy.host.exec({{{:?},{:?}}},{{timeout_ms=100,ttl_ms=1000}}); return a.stdout..b.stdout end),
  environment=zone(function(ctx) local x=pixy.host.exec({{{:?}}},{{timeout_ms=100,ttl_ms=1000,env={{TOKEN=ctx.values.token}}}}); return x.stdout end),
  request_environment=zone(function() local x=pixy.host.exec({{{:?}}},{{timeout_ms=100,ttl_ms=1000}}); return x.stdout end),
  descendant=zone(function() local x=pixy.host.exec({{{:?}}},{{timeout_ms=100}}); return x.stdout end),
}}}})
"#,
        script.display().to_string(),
        counter.display().to_string(),
        script.display().to_string(),
        counter.display().to_string(),
        env_script.display().to_string(),
        env_script.display().to_string(),
        descendant_script.display().to_string()
    );
    let engine = engine(&root, source);
    assert_eq!(text(&engine, "timeout", &[]), "true");
    assert_eq!(text(&engine, "cap", &[]), "true");
    assert_eq!(text(&engine, "cache", &[]), "cachedcached");
    assert_eq!(fs::read_to_string(counter).expect("counter"), "x");
    assert_eq!(text(&engine, "environment", &[("token", json!("a"))]), "a");
    assert_eq!(text(&engine, "environment", &[("token", json!("b"))]), "b");
    let mut context = RenderContext::default();
    context.env.insert("TOKEN".into(), Some("request".into()));
    assert_eq!(
        text_with(&engine, "request_environment", context.clone(), 0),
        "request"
    );
    context.env.insert("TOKEN".into(), None);
    assert_eq!(text_with(&engine, "request_environment", context, 1), "");
    let started = std::time::Instant::now();
    assert_eq!(text(&engine, "descendant", &[]), "done");
    assert!(started.elapsed() < std::time::Duration::from_secs(1));
    fs::remove_dir_all(root).expect("cleanup");
}

#[test]
fn file_host_is_bounded_to_trusted_roots() {
    let root = root();
    fs::write(root.join("small"), "ok").expect("small file");
    fs::write(root.join("large"), vec![b'x'; 64 * 1024 + 1]).expect("large file");
    let outside = root.with_extension("outside");
    fs::write(&outside, "outside").expect("outside file");
    std::os::unix::fs::symlink(&outside, root.join("escape")).expect("escape symlink");
    let source = r#"
local pixy=require("pixy")
local function read(ctx)
  local ok,value=pcall(pixy.host.read,ctx.values.path)
  return tostring(ok)..":"..(ok and tostring(value) or "error")
end
return pixy.config({zones={read=pixy.zone({pixy.segment("value",read)})}})
"#;
    let engine = engine(&root, source.into());
    assert_eq!(
        text(&engine, "read", &[("path", json!("small"))]),
        "true:ok"
    );
    assert_eq!(
        text(&engine, "read", &[("path", json!("missing"))]),
        "true:nil"
    );
    for path in ["../outside", "large", "escape"] {
        assert!(text(&engine, "read", &[("path", json!(path))]).starts_with("false:"));
    }
    fs::remove_file(outside).expect("outside cleanup");
    fs::remove_dir_all(root).expect("cleanup");
}

#[test]
fn every_lua_provider_handles_happy_absent_and_malformed_fixtures() {
    let root = root();
    fs::write(root.join("uptime"), "10.5 1\n").expect("uptime");
    fs::write(
        root.join("meminfo"),
        "MemTotal: 100 kB\nMemAvailable: 40 kB\n",
    )
    .expect("memory");
    fs::write(root.join("stat"), "cpu 1 2 3 4\n").expect("cpu");
    fs::write(root.join("capacity"), "77\n").expect("capacity");
    fs::write(root.join("battery-status"), "Charging\n").expect("battery status");
    fs::write(root.join("netdev"), "eth0: 10 0 0 0 0 0 0 0 20 0\n").expect("network");
    let source = r#"
local pixy=require("pixy")
local system=require("pixy.segments.system")
local git=require("pixy.segments.git")
local shell=require("pixy.segments.shell")
local function show(value) return value == nil and "nil" or tostring(value) end
local function zone(renderer) return pixy.zone({pixy.segment("value",renderer)}) end
return pixy.config({zones={
  time=zone(function(ctx) return show(system.time(ctx)) end),
  identity=zone(function() return show(system.hostname())..":"..show(system.username()) end),
  battery=zone(function(ctx) local x=system.battery(ctx); return x and show(x.percent)..":"..show(x.status) or "nil" end),
  speed=zone(function(ctx) local x=system.network_speed(ctx); return x and show(x.received_per_second)..":"..show(x.sent_per_second) or "nil" end),
  random=zone(function(ctx) return show(system.random(ctx,ctx.values.words)) end),
  sudo=zone(function(ctx) return show(system.sudo(ctx)) end),
  shell=zone(function(ctx) return table.concat({show(shell.directory(ctx)),show(shell.status(ctx)),show(shell.duration(ctx)),show(shell.jobs(ctx)),show(shell.last_command(ctx)),show(shell.running(ctx) ~= nil)},":") end),
  git=zone(function(ctx) return show(git.branch(ctx))..":"..show(git.status(ctx)) end),
  absent=zone(function(ctx) return table.concat({
    show(system.uptime(ctx)),show(system.memory(ctx)),show(system.cpu(ctx)),show(system.battery(ctx)),show(system.network(ctx)),
    show(system.random(ctx,{})),show(git.branch(ctx)),show(git.status(ctx)),show(shell.directory(ctx)),show(shell.status(ctx))
  },":") end),
  mac=zone(function(ctx)
    local boot=system.parse_macos_boottime(ctx.values.boot,200)
    local memory=system.parse_macos_memory(ctx.values.memory)
    local cpu=system.parse_macos_cpu(ctx.values.cpu)
    local battery=system.parse_macos_battery(ctx.values.battery)
    local network=system.parse_macos_network(ctx.values.network)
    return table.concat({show(boot),memory and show(memory.total_bytes) or "nil",cpu and show(cpu.total)..":"..show(cpu.idle) or "nil",battery and show(battery.percent) or "nil",network and show(network.received)..":"..show(network.sent) or "nil"},":")
  end),
}})
"#;
    let engine = engine(&root, source.into());
    let mut context = RenderContext::default();
    context.values.insert("cwd".into(), json!("/tmp/project"));
    context.values.insert("status".into(), json!(7));
    context.values.insert("duration_ms".into(), json!(42));
    context.values.insert("jobs".into(), json!(3));
    context.values.insert("started_at_ms".into(), json!(0));
    context.env.insert("HOSTNAME".into(), Some("host".into()));
    context.env.insert("USER".into(), Some("user".into()));
    context.values.insert("uptime_path".into(), json!("uptime"));
    context
        .values
        .insert("meminfo_path".into(), json!("meminfo"));
    context.values.insert("stat_path".into(), json!("stat"));
    context
        .values
        .insert("battery_capacity_path".into(), json!("capacity"));
    context
        .values
        .insert("battery_status_path".into(), json!("battery-status"));
    context.values.insert("netdev_path".into(), json!("netdev"));
    context.values.insert("words".into(), json!(["a", "b"]));
    context.values.insert("sudo".into(), json!(true));
    context
        .values
        .insert("last_command".into(), json!("make test"));
    context.values.insert("running".into(), json!(true));
    context.values.insert("git_branch".into(), json!("main"));
    context.values.insert("git_status".into(), json!("dirty"));
    assert_eq!(text_with(&engine, "time", context.clone(), 2_000), "2");
    assert_eq!(
        text_with(&engine, "identity", context.clone(), 2_000),
        "host:user"
    );
    assert_eq!(
        text_with(&engine, "battery", context.clone(), 2_000),
        "77:Charging"
    );
    assert_eq!(text_with(&engine, "random", context.clone(), 2_000), "a");
    assert_eq!(text_with(&engine, "sudo", context.clone(), 2_000), "true");
    assert_eq!(
        text_with(&engine, "shell", context.clone(), 2_000),
        "project:7:42:3:make test:true"
    );
    assert_eq!(
        text_with(&engine, "git", context.clone(), 2_000),
        "main:dirty"
    );
    assert_eq!(text_with(&engine, "speed", context.clone(), 2_000), "nil");
    fs::write(root.join("netdev"), "eth0: 30 0 0 0 0 0 0 0 50 0\n").expect("network");
    assert_eq!(
        text_with(&engine, "speed", context.clone(), 3_000),
        "20.0:30.0"
    );

    let mut absent = RenderContext::default();
    absent.env.insert("HOSTNAME".into(), None);
    absent.env.insert("USER".into(), None);
    absent.env.insert("LOGNAME".into(), None);
    for key in [
        "uptime_path",
        "meminfo_path",
        "stat_path",
        "battery_capacity_path",
        "battery_status_path",
        "netdev_path",
    ] {
        absent.values.insert(key.into(), json!("missing"));
    }
    absent.values.insert("git_branch".into(), json!(false));
    absent.values.insert("git_status".into(), json!(false));
    assert_eq!(
        text_with(&engine, "absent", absent.clone(), 2_000),
        "nil:nil:nil:nil:nil:nil:nil:nil:nil:nil"
    );
    assert_eq!(text_with(&engine, "identity", absent, 2_000), "nil:nil");

    let mut malformed = context.clone();
    fs::write(root.join("uptime"), "bad\n").expect("uptime");
    fs::write(root.join("meminfo"), "MemTotal: 1 kB\nMemAvailable: 2 kB\n").expect("memory");
    fs::write(root.join("stat"), "cpu bad\n").expect("cpu");
    fs::write(root.join("capacity"), "101\n").expect("capacity");
    fs::write(root.join("netdev"), "header only\n").expect("network");
    malformed.values.insert("git_branch".into(), json!(42));
    malformed
        .values
        .insert("git_status".into(), json!("unknown"));
    assert_eq!(
        text_with(&engine, "absent", malformed, 2_000),
        "nil:nil:nil:nil:nil:nil:nil:nil:project:7"
    );

    let mut mac = RenderContext::default();
    mac.values
        .insert("boot".into(), json!("{ sec = 100, usec = 0 }"));
    mac.values.insert("memory".into(), json!("1024\n"));
    mac.values.insert("cpu".into(), json!("1 2 3 4 5"));
    mac.values.insert(
        "battery".into(),
        json!(
            "Now drawing from 'Battery Power'\n -InternalBattery-0 77%; discharging; 3:00 remaining"
        ),
    );
    mac.values
        .insert("network".into(), json!("en0 1500 link aa 1 0 10 2 0 20 0"));
    assert_eq!(
        text_with(&engine, "mac", mac, 2_000),
        "100:1024:15:5:77:10:20"
    );
    let mut malformed_mac = RenderContext::default();
    malformed_mac
        .values
        .insert("boot".into(), json!("{ sec = 300, usec = 0 }"));
    malformed_mac.values.insert("memory".into(), json!("bad"));
    malformed_mac.values.insert("cpu".into(), json!("1 2 3 4"));
    malformed_mac.values.insert("battery".into(), json!("101%"));
    malformed_mac
        .values
        .insert("network".into(), json!("header"));
    assert_eq!(
        text_with(&engine, "mac", malformed_mac, 2_000),
        "nil:nil:nil:nil:nil"
    );
    fs::remove_dir_all(root).expect("cleanup");
}

#[test]
fn git_provider_uses_bounded_cached_fixture_processes() {
    let root = root();
    let bin = root.join("bin");
    fs::create_dir_all(&bin).expect("bin");
    let git = bin.join("git");
    let counter = root.join("git-count");
    fs::write(
        &git,
        format!(
            "#!/bin/sh\nprintf '%s\\n' \"$1\" >> {:?}\ncase \"$1\" in branch) printf 'fixture\\n';; status) printf ' M file\\n';; esac\n",
            counter.display().to_string()
        ),
    )
    .expect("git fixture");
    fs::set_permissions(&git, fs::Permissions::from_mode(0o755)).expect("mode");
    let engine = engine(
        &root,
        r#"
local pixy=require("pixy")
local git=require("pixy.segments.git")
local function show(value) return value == nil and "nil" or tostring(value) end
return pixy.config({zones={git=pixy.zone({pixy.segment("value",function(ctx) return show(git.branch(ctx))..":"..show(git.status(ctx)) end)})}})
"#
        .into(),
    );
    let mut context = RenderContext::default();
    context
        .values
        .insert("cwd".into(), json!(root.display().to_string()));
    context
        .env
        .insert("PATH".into(), Some(bin.display().to_string()));
    assert_eq!(
        text_with(&engine, "git", context.clone(), 0),
        "fixture:dirty"
    );
    assert_eq!(
        text_with(&engine, "git", context.clone(), 1),
        "fixture:dirty"
    );
    assert_eq!(
        fs::read_to_string(&counter)
            .expect("counter")
            .lines()
            .count(),
        2
    );
    std::thread::sleep(std::time::Duration::from_millis(300));
    assert_eq!(
        text_with(&engine, "git", context.clone(), 2),
        "fixture:dirty"
    );
    assert_eq!(
        fs::read_to_string(&counter)
            .expect("counter")
            .lines()
            .count(),
        4
    );

    fs::write(&git, "#!/bin/sh\nsleep 1\n").expect("timeout fixture");
    fs::set_permissions(&git, fs::Permissions::from_mode(0o755)).expect("mode");
    std::thread::sleep(std::time::Duration::from_millis(300));
    let started = std::time::Instant::now();
    assert_eq!(text_with(&engine, "git", context, 3), "nil:nil");
    assert!(started.elapsed() < std::time::Duration::from_secs(1));
    fs::remove_dir_all(root).expect("cleanup");
}
