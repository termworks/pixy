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
        name: "@hardening.lua".into(),
        source: body.into(),
        directory: root,
        path: None,
    };
    Engine::load(source, &paths).expect("load engine")
}

/// Two zones: `hostile` runs `body`, `sane` always renders `ok`.
fn paired(body: &str) -> Engine {
    engine(&format!(
        "local pixy=require('pixy')
         return pixy.config({{zones={{
           hostile=pixy.zone({{pixy.segment('value', function() {body} end)}}),
           sane=pixy.zone({{pixy.segment('value', function() return 'ok' end)}}),
         }}}})"
    ))
}

fn request(select: &str) -> RenderRequest {
    RenderRequest {
        select: vec![select.into()],
        mode: RenderMode::Line,
        target: Some(LineTarget::Plain),
        now_ms: Some(0),
        ..RenderRequest::default()
    }
}

fn text(output: &RenderOutput) -> &str {
    match output {
        RenderOutput::Line { text, .. } => text,
        _ => panic!("expected a line"),
    }
}

/// The engine reuses one executor across renders, so a render that is killed
/// mid-flight must not leave the next one wedged.
#[test]
fn engine_survives_every_hostile_segment_and_still_renders() {
    for body in [
        "while true do end",
        "local t={} while true do t[#t+1]=string.rep('x',4096) end",
        "local function f(n) return f(n+1) end return f(1)",
        "error('boom')",
        "error(setmetatable({},{__tostring=function() error('nested') end}))",
        "return nil",
        "return {kind='unknown'}",
        "return ('x'):rep(1 << 20)",
        "local co=coroutine.create(function() coroutine.yield(1) end) coroutine.resume(co) return 'y'",
        "return setmetatable({},{__index=function() error('trap') end})",
    ] {
        let engine = paired(body);
        let _ = engine.render(request("hostile"));
        let recovered = engine
            .render(request("sane"))
            .unwrap_or_else(|error| panic!("engine wedged after {body:?}: {error}"));
        assert_eq!(text(&recovered), "ok", "wrong output after {body:?}");
    }
}

/// Repeatedly failing then succeeding must stay stable, not degrade.
#[test]
fn alternating_failure_and_success_stays_stable() {
    let engine = paired("while true do end");
    for round in 0..25 {
        assert!(
            engine.render(request("hostile")).is_err(),
            "round {round} should have hit the deadline"
        );
        let output = engine
            .render(request("sane"))
            .unwrap_or_else(|error| panic!("round {round} wedged: {error}"));
        assert_eq!(text(&output), "ok");
    }
}

/// Every hostile segment must be stopped well inside a human-noticeable budget.
#[test]
fn hostile_segments_are_bounded_in_wall_clock() {
    for body in [
        "while true do end",
        "local t={} while true do t[#t+1]=string.rep('x',4096) end",
        "for i=1,1e15 do end",
        "local s='' while true do s=s..'xxxxxxxx' end",
    ] {
        let engine = paired(body);
        let started = Instant::now();
        assert!(engine.render(request("hostile")).is_err(), "{body:?}");
        assert!(
            started.elapsed() < Duration::from_secs(1),
            "{body:?} ran for {:?}",
            started.elapsed()
        );
    }
}

/// `#t` is unspecified for a table with holes, so no Lua value may bound a
/// host-side loop. Sparse argv and sparse output lists must terminate.
#[test]
fn sparse_lua_lists_never_run_away() {
    for body in [
        "pixy.host.exec({[1]='true',[1<<62]='x'}) return 'done'",
        "pixy.host.exec({'true',[1<<40]='x'}) return 'done'",
        "pixy.host.exec({}) return 'done'",
        "local a={} for i=1,500 do a[i]='x' end pixy.host.exec(a) return 'done'",
    ] {
        let engine = engine(&format!(
            "local pixy=require('pixy')
             return pixy.config({{zones={{probe=pixy.zone({{
               pixy.segment('value', function() {body} end)}})}}}})"
        ));
        let started = Instant::now();
        let _ = engine.render(request("probe"));
        assert!(
            started.elapsed() < Duration::from_secs(2),
            "{body:?} ran for {:?}",
            started.elapsed()
        );
    }
}

/// A segment may return anything. Each value either renders or produces a clean
/// error; none may panic, and none may leave the engine unusable.
#[test]
fn unsupported_segment_values_are_rejected_cleanly() {
    for body in [
        "return {}",
        "return true",
        "return {kind='unknown'}",
        "return {kind='pixy_run', runs='not a list'}",
        "return {kind='pixy_line', text={}}",
        "return function() end",
        "return coroutine.create(function() end)",
    ] {
        let engine = paired(body);
        assert!(
            engine.render(request("hostile")).is_err(),
            "accepted unsupported segment value {body:?}"
        );
        assert_eq!(
            text(&engine.render(request("sane")).expect("recover")),
            "ok"
        );
    }
}

/// Scalars coerce to text rather than failing, and stay bounded.
#[test]
fn scalar_segment_values_render_as_text() {
    for (body, expected) in [
        ("return 42", "42"),
        ("return 4.5", "4.5"),
        ("return 'plain'", "plain"),
        ("return ''", ""),
    ] {
        let engine = paired(body);
        let output = engine
            .render(request("hostile"))
            .unwrap_or_else(|error| panic!("{body:?} failed: {error}"));
        assert_eq!(text(&output), expected, "for {body:?}");
    }
}

/// The render deadline bounds Lua execution, not host I/O. A provider slower
/// than the deadline — `git status` in a large repository — must still render.
#[test]
fn a_provider_slower_than_the_render_deadline_still_renders() {
    let engine = paired(
        "local r = pixy.host.exec({'sleep','0.05'}, {timeout_ms=500})
         return 'status=' .. tostring(r.status)",
    );
    let output = engine
        .render(request("hostile"))
        .expect("slow provider should render");
    assert_eq!(text(&output), "status=0");
}

/// Excluding I/O from the deadline must not make a render unbounded: the
/// per-render I/O budget still caps total blocking time.
#[test]
fn total_host_io_stays_bounded_across_calls() {
    let engine = paired(
        "local last = 0
         for _ = 1, 8 do
           local r = pcall(pixy.host.exec, {'sleep','0.4'}, {timeout_ms=2000})
           if not r then break end
           last = last + 1
         end
         return 'calls=' .. tostring(last)",
    );
    let started = Instant::now();
    let _ = engine.render(request("hostile"));
    assert!(
        started.elapsed() < Duration::from_secs(5),
        "host I/O ran unbounded for {:?}",
        started.elapsed()
    );
}

/// Lua compute is still bounded when a segment mixes I/O with a runaway loop.
#[test]
fn compute_deadline_survives_mixed_io_and_looping() {
    let engine = paired(
        "pixy.host.exec({'true'}, {timeout_ms=200})
         while true do end",
    );
    let started = Instant::now();
    assert!(engine.render(request("hostile")).is_err());
    assert!(
        started.elapsed() < Duration::from_secs(1),
        "looped for {:?}",
        started.elapsed()
    );
    assert_eq!(
        text(&engine.render(request("sane")).expect("recover")),
        "ok"
    );
}

/// A hostile segment must not be able to reach the filesystem or the process
/// table through the standard library.
#[test]
fn sandbox_holds_under_probing() {
    let engine = paired(
        "local escaped = pcall(require('pixy').host.read, '/etc/shadow')
         return tostring(io == nil and os == nil and debug == nil
            and dofile == nil and loadfile == nil
            and package.loadlib == nil and package.cpath == nil
            and not escaped)",
    );
    let output = engine.render(request("hostile")).expect("probe renders");
    assert_eq!(text(&output), "true");
}

/// Selectors are attacker-shaped input in the CLI; none may panic the engine.
#[test]
fn adversarial_selectors_are_rejected_not_fatal() {
    let engine = paired("return 'ok'");
    for selector in [
        "",
        ".",
        "..",
        "hostile.",
        ".value",
        "hostile.value.extra",
        "hostile/../etc",
        "hostile\0value",
        "hostile value",
        &"a".repeat(4096),
    ] {
        let request = RenderRequest {
            select: vec![selector.into()],
            ..request("hostile")
        };
        let _ = engine.render(request);
    }
    assert_eq!(
        text(&engine.render(request("sane")).expect("recover")),
        "ok"
    );
}

/// Sprite surfaces are the heaviest supported render and drive `pixy stream`
/// animation. Decoded assets are memoised, so repeated frames must not drift.
#[test]
fn repeated_surface_frames_stay_bounded() {
    let engine = engine(
        "local pixy=require('pixy')
         return pixy.config({zones={
           overlay=pixy.zone({pixy.segment('sprite', function()
             return pixy.sprite({pack='pokemon', name='regular/pikachu'})
           end)}),
         }})",
    );
    let mut frames = Vec::new();
    for frame in 0..60 {
        let request = RenderRequest {
            select: vec!["overlay.sprite".into()],
            mode: RenderMode::Surface,
            target: None,
            width: 80,
            height: 40,
            now_ms: Some(frame * 16),
            ..RenderRequest::default()
        };
        let started = Instant::now();
        engine
            .render(request)
            .unwrap_or_else(|error| panic!("frame {frame} failed: {error}"));
        frames.push(started.elapsed());
    }
    // Compared as a ratio rather than against a wall-clock bound: both halves
    // feel CPU contention equally, so this measures the code and not the
    // machine. A memoised sprite must not get more expensive as it animates.
    let half = frames.len() / 2;
    let early: Duration = frames[..half].iter().sum();
    let late: Duration = frames[half..].iter().sum();
    assert!(
        late <= early * 4,
        "surface frames drifted: first half {early:?}, second half {late:?}"
    );
}
