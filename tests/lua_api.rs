use pixy::config::{ConfigSource, Paths};
use pixy::{Engine, LineTarget, RenderOutput, RenderRequest};

fn render(source: &str, request: RenderRequest) -> RenderOutput {
    let root = std::env::temp_dir();
    let paths = Paths {
        config_dir: root.clone(),
        cache_dir: root.clone(),
        data_dir: root.clone(),
    };
    let source = ConfigSource {
        name: "@lua-api.lua".into(),
        source: source.into(),
        directory: root,
        path: None,
    };
    Engine::load(source, &paths)
        .expect("engine")
        .render(request)
        .expect("render")
}

fn bundled() -> Engine {
    let root = std::env::temp_dir();
    let paths = Paths {
        config_dir: root.clone(),
        cache_dir: root.clone(),
        data_dir: root.clone(),
    };
    Engine::load(
        ConfigSource {
            name: "@pixy/default.lua".into(),
            source: pixy::config::DEFAULT_CONFIG.into(),
            directory: root,
            path: None,
        },
        &paths,
    )
    .expect("bundled engine")
}

fn styled(target: LineTarget) -> RenderOutput {
    let root = std::env::temp_dir();
    let paths = Paths {
        config_dir: root.clone(),
        cache_dir: root.clone(),
        data_dir: root.clone(),
    };
    let source = ConfigSource {
        name: "@style.lua".into(),
        source: "local pixy=require('pixy'); return pixy.config({zones={x=pixy.zone({pixy.segment('text',function() return pixy.text('hi',{fg=1,bold=true}) end)})}})".into(),
        directory: root,
        path: None,
    };
    Engine::load(source, &paths)
        .expect("engine")
        .render(RenderRequest {
            select: vec!["x".into()],
            target: Some(target),
            now_ms: Some(0),
            ..RenderRequest::default()
        })
        .expect("render")
}

#[test]
fn encodes_line_targets_in_lua() {
    assert!(matches!(styled(LineTarget::Plain), RenderOutput::Line { text, .. } if text == "hi"));
    assert!(
        matches!(styled(LineTarget::Ansi), RenderOutput::Line { text, .. } if text == "\u{1b}[38;5;1;1mhi\u{1b}[0m")
    );
    assert!(
        matches!(styled(LineTarget::Bash), RenderOutput::Line { text, .. } if text == "\\[\u{1b}[38;5;1;1m\\]hi\\[\u{1b}[0m\\]")
    );
    assert!(
        matches!(styled(LineTarget::Zsh), RenderOutput::Line { text, .. } if text == "%{\u{1b}[38;5;1;1m%}hi%{\u{1b}[0m%}")
    );
}

#[test]
fn run_styles_are_hexe_portable_without_changing_ansi() {
    let source = r#"
local pixy=require("pixy")
return pixy.config({zones={style=pixy.zone({
  pixy.segment("rgb",function() return pixy.text("X",{fg={255,85,0},bg={20,30,40}}) end),
  pixy.segment("palette",function() return pixy.text("X",{fg=250,bg=237}) end),
  pixy.segment("named",function() return pixy.text("X",{fg="red"}) end),
  pixy.segment("reversed",function() return pixy.text("X",{fg={255,85,0},bg={20,30,40},reverse=true}) end),
  pixy.segment("partial",function() return pixy.text("X",{fg=1,reverse=true}) end),
  pixy.segment("defaults",function() return pixy.text("X",{fg="default",bg="default",bold=true}) end),
  pixy.segment("ansi",function()
    return pixy.row({
      pixy.text("X",{fg={255,85,0},bg={20,30,40},bold=true,reverse=true}),
      pixy.text("Y",{fg="default",bg="default"}),
    })
  end),
  pixy.segment("bare",function() return pixy.row({pixy.text("X",{reverse=true}),pixy.text("Y")}) end),
})}})
"#;
    let run_style = |name: &str| {
        let output = render(
            source,
            RenderRequest {
                select: vec![format!("style.{name}")],
                mode: pixy::RenderMode::Run,
                target: None,
                now_ms: Some(0),
                ..RenderRequest::default()
            },
        );
        let RenderOutput::Run { runs, .. } = output else {
            panic!("run output");
        };
        runs.first().expect("run").style.clone()
    };
    assert_eq!(run_style("rgb"), "fg:#ff5500 bg:#141e28");
    assert_eq!(run_style("palette"), "fg:250 bg:237");
    assert_eq!(run_style("named"), "fg:1");
    assert_eq!(run_style("reversed"), "fg:#141e28 bg:#ff5500");
    assert_eq!(run_style("partial"), "fg:1");
    assert_eq!(run_style("defaults"), "bold");

    let ansi = render(
        source,
        RenderRequest {
            select: vec!["style.ansi".into()],
            target: Some(LineTarget::Ansi),
            now_ms: Some(0),
            ..RenderRequest::default()
        },
    );
    assert!(
        matches!(ansi, RenderOutput::Line { text, .. } if text == "\u{1b}[38;2;255;85;0;48;2;20;30;40;1;7mX\u{1b}[0m\u{1b}[39;49mY\u{1b}[0m")
    );

    let bare = render(
        source,
        RenderRequest {
            select: vec!["style.bare".into()],
            target: Some(LineTarget::Ansi),
            now_ms: Some(0),
            ..RenderRequest::default()
        },
    );
    assert!(matches!(bare, RenderOutput::Line { text, .. } if text == "\u{1b}[7mX\u{1b}[0mY"));
}

#[test]
fn spinner_is_timestamp_deterministic() {
    let first = bundled()
        .render(RenderRequest {
            select: vec!["activity.spinner".into()],
            target: Some(LineTarget::Plain),
            now_ms: Some(160),
            ..RenderRequest::default()
        })
        .expect("first");
    let second = bundled()
        .render(RenderRequest {
            select: vec!["activity.spinner".into()],
            target: Some(LineTarget::Plain),
            now_ms: Some(160),
            ..RenderRequest::default()
        })
        .expect("second");
    assert_eq!(first, second);
}

#[test]
fn sprite_surface_reports_next_frame() {
    let mut request = RenderRequest {
        select: vec!["mascot".into()],
        mode: pixy::RenderMode::Surface,
        target: None,
        width: 8,
        height: 3,
        now_ms: Some(0),
        ..RenderRequest::default()
    };
    let engine = bundled();
    let first = engine.render(request.clone()).expect("first");
    request.now_ms = Some(200);
    let second = engine.render(request).expect("second");
    assert_eq!(first.next_frame_ms(), Some(200));
    assert_eq!(second.next_frame_ms(), Some(400));
    assert_ne!(first, second);
}

#[test]
fn missing_optional_sprite_pack_renders_nothing() {
    let output = render(
        "local pixy=require('pixy'); return pixy.config({zones={x=pixy.zone({pixy.segment('sprite',function() return pixy.sprite({pack='missing',name='sprite.txt'}) end)})}})",
        RenderRequest {
            select: vec!["x".into()],
            target: Some(LineTarget::Plain),
            now_ms: Some(0),
            ..RenderRequest::default()
        },
    );
    assert!(matches!(output, RenderOutput::Line { text, width: 0, .. } if text.is_empty()));
}

#[test]
fn static_sprite_does_not_schedule_redundant_frames() {
    let output = render(
        "local pixy=require('pixy'); return pixy.config({zones={x=pixy.zone({pixy.segment('sprite',function() return pixy.sprite({frames={'X'}}) end)})}})",
        RenderRequest {
            select: vec!["x".into()],
            mode: pixy::RenderMode::Surface,
            target: None,
            width: 1,
            height: 1,
            now_ms: Some(0),
            ..RenderRequest::default()
        },
    );
    assert!(
        matches!(output, RenderOutput::Surface { ansi, next_frame_ms: None, .. } if ansi == "X")
    );
}

#[test]
fn ansi_sprite_accepts_sgr_and_rejects_other_controls() {
    let source = r#"
local pixy=require("pixy")
return pixy.config({zones={sprite=pixy.zone({
  pixy.segment("safe",function()
    return pixy.sprite({frames={" \27[38;2;246;213;49m▄\27[48;2;197;164;41m▀\27[0m "},format="ansi"})
  end),
  pixy.segment("unsafe",function() return pixy.sprite({frames={"\27[2JX"},format="ansi"}) end),
})}})
"#;
    let output = render(
        source,
        RenderRequest {
            select: vec!["sprite.safe".into()],
            mode: pixy::RenderMode::Surface,
            target: None,
            width: 4,
            height: 1,
            now_ms: Some(0),
            ..RenderRequest::default()
        },
    );
    let RenderOutput::Surface { ansi, .. } = output else {
        panic!("sprite surface");
    };
    assert!(ansi.contains("38;2;246;213;49"));
    assert!(ansi.contains("48;2;197;164;41"));
    assert!(ansi.starts_with("\u{1b}[1C"));

    let root = std::env::temp_dir();
    let paths = Paths {
        config_dir: root.clone(),
        cache_dir: root.clone(),
        data_dir: root.clone(),
    };
    let engine = Engine::load(
        ConfigSource {
            name: "@unsafe-sprite.lua".into(),
            source: source.into(),
            directory: root,
            path: None,
        },
        &paths,
    )
    .expect("engine");
    assert!(
        engine
            .render(RenderRequest {
                select: vec!["sprite.unsafe".into()],
                mode: pixy::RenderMode::Surface,
                target: None,
                width: 4,
                height: 1,
                now_ms: Some(0),
                ..RenderRequest::default()
            })
            .is_err()
    );
}

#[test]
fn run_and_surface_match_exact_golden_json() {
    let run = render(
        r#"
local pixy=require("pixy")
return pixy.config({zones={golden=pixy.zone({pixy.segment("value",function()
  return pixy.row({pixy.text("A",{fg="red",bold=true}),pixy.text("界",{fg={1,2,3},underline=true})})
end)})}})
"#,
        RenderRequest {
            select: vec!["golden".into()],
            mode: pixy::RenderMode::Run,
            target: None,
            width: 8,
            now_ms: Some(0),
            ..RenderRequest::default()
        },
    );
    assert_eq!(
        serde_json::to_string(&run).expect("run JSON"),
        include_str!("fixtures/golden/run.json").trim()
    );

    let surface = render(
        r#"
local pixy=require("pixy")
return pixy.config({zones={golden=pixy.zone({pixy.segment("value",function(ctx)
  return pixy.sprite({frames={" A "},interval_ms=100,started_at_ms=ctx.values.started_at_ms})
end)})}})
"#,
        RenderRequest {
            select: vec!["golden".into()],
            mode: pixy::RenderMode::Surface,
            target: None,
            width: 3,
            height: 1,
            now_ms: Some(0),
            ..RenderRequest::default()
        },
    );
    assert_eq!(
        serde_json::to_string(&surface).expect("surface JSON"),
        include_str!("fixtures/golden/surface.json").trim()
    );
}

#[test]
fn surface_rows_anchor_offsets_and_edges_are_bounded() {
    let source = r#"
local pixy=require("pixy")
return pixy.config({zones={surface=pixy.zone({
  pixy.segment("grid",function() return pixy.row({pixy.column({"A","B"}),pixy.column({"12","34"})}) end),
  pixy.segment("anchor",function() return pixy.sprite({frames={"X"},anchor="center",x=1,y=-1,transparent=false}) end),
  pixy.segment("wide",function() return "a界" end),
  pixy.segment("unicode",function() return "🙂é" end),
  pixy.segment("padded",function() return pixy.truncate(pixy.pad("abc",{left=1,right=1}),4,"…") end),
  pixy.segment("blank",function() return pixy.sprite({frames={"A\n\nB"},transparent=false}) end),
  pixy.segment("topcenter",function() return pixy.sprite({frames={"X"},anchor="top-center",transparent=false}) end),
  pixy.segment("centerleft",function() return pixy.sprite({frames={"X"},anchor="center-left",transparent=false}) end),
  pixy.segment("family",function() return "👨‍👩‍👧‍👦X" end),
  pixy.segment("leftclip",function() return pixy.sprite({frames={"AB"},x=-1,transparent=false}) end),
})}})
"#;
    let grid = render(
        source,
        RenderRequest {
            select: vec!["surface.grid".into()],
            mode: pixy::RenderMode::Surface,
            target: None,
            width: 3,
            height: 2,
            now_ms: Some(0),
            ..RenderRequest::default()
        },
    );
    assert!(
        matches!(grid, RenderOutput::Surface { ansi, width: 3, height: 2, .. } if ansi == "A12\nB34")
    );

    let zero = render(
        source,
        RenderRequest {
            select: vec!["surface.grid".into()],
            mode: pixy::RenderMode::Surface,
            target: None,
            width: 0,
            height: 0,
            now_ms: Some(0),
            ..RenderRequest::default()
        },
    );
    assert!(
        matches!(zero, RenderOutput::Surface { ansi, width: 0, height: 0, .. } if ansi.is_empty())
    );

    let anchor = render(
        source,
        RenderRequest {
            select: vec!["surface.anchor".into()],
            mode: pixy::RenderMode::Surface,
            target: None,
            width: 5,
            height: 3,
            now_ms: Some(0),
            ..RenderRequest::default()
        },
    );
    assert!(
        matches!(anchor, RenderOutput::Surface { ansi, width: 4, height: 1, .. } if ansi == "\u{1b}[3CX")
    );

    let wide = render(
        source,
        RenderRequest {
            select: vec!["surface.wide".into()],
            target: Some(LineTarget::Plain),
            width: 2,
            now_ms: Some(0),
            ..RenderRequest::default()
        },
    );
    assert!(matches!(wide, RenderOutput::Line { text, width: 1, .. } if text == "a"));

    let unicode = render(
        source,
        RenderRequest {
            select: vec!["surface.unicode".into()],
            target: Some(LineTarget::Plain),
            width: 3,
            now_ms: Some(0),
            ..RenderRequest::default()
        },
    );
    assert!(matches!(unicode, RenderOutput::Line { text, width: 3, .. } if text == "🙂é"));

    let padded = render(
        source,
        RenderRequest {
            select: vec!["surface.padded".into()],
            target: Some(LineTarget::Plain),
            width: 4,
            now_ms: Some(0),
            ..RenderRequest::default()
        },
    );
    assert!(matches!(padded, RenderOutput::Line { text, width: 4, .. } if text == " ab…"));

    let blank = render(
        source,
        RenderRequest {
            select: vec!["surface.blank".into()],
            mode: pixy::RenderMode::Surface,
            target: None,
            width: 1,
            height: 3,
            now_ms: Some(0),
            ..RenderRequest::default()
        },
    );
    assert!(
        matches!(blank, RenderOutput::Surface { ansi, width: 1, height: 3, .. } if ansi == "A\n \nB")
    );

    let topcenter = render(
        source,
        RenderRequest {
            select: vec!["surface.topcenter".into()],
            mode: pixy::RenderMode::Surface,
            target: None,
            width: 5,
            height: 3,
            now_ms: Some(0),
            ..RenderRequest::default()
        },
    );
    assert!(
        matches!(topcenter, RenderOutput::Surface { ansi, height: 1, .. } if ansi == "\u{1b}[2CX")
    );

    let centerleft = render(
        source,
        RenderRequest {
            select: vec!["surface.centerleft".into()],
            mode: pixy::RenderMode::Surface,
            target: None,
            width: 5,
            height: 3,
            now_ms: Some(0),
            ..RenderRequest::default()
        },
    );
    assert!(
        matches!(centerleft, RenderOutput::Surface { ansi, height: 2, .. } if ansi == "\u{1b}[1C\nX")
    );

    let family = render(
        source,
        RenderRequest {
            select: vec!["surface.family".into()],
            target: Some(LineTarget::Plain),
            width: 2,
            now_ms: Some(0),
            ..RenderRequest::default()
        },
    );
    assert!(matches!(family, RenderOutput::Line { text, width: 2, .. } if text == "👨‍👩‍👧‍👦"));

    let leftclip = render(
        source,
        RenderRequest {
            select: vec!["surface.leftclip".into()],
            mode: pixy::RenderMode::Surface,
            target: None,
            width: 2,
            height: 1,
            now_ms: Some(0),
            ..RenderRequest::default()
        },
    );
    assert!(matches!(leftclip, RenderOutput::Surface { ansi, width: 1, .. } if ansi == "B"));
}

#[test]
fn procedural_animation_uses_timestamp_and_deadline() {
    let source = r#"
local pixy=require("pixy")
return pixy.config({zones={animated=pixy.zone({pixy.segment("value",function(ctx)
  return pixy.animate({interval_ms=50,started_at_ms=0,callback=function(now)
    return tostring(math.floor(now.now_ms / 50) % 2)
  end})
end)})}})
"#;
    for (now, expected, next) in [(0, "0", 50), (50, "1", 100), (100, "0", 150)] {
        let output = render(
            source,
            RenderRequest {
                select: vec!["animated".into()],
                target: Some(LineTarget::Plain),
                now_ms: Some(now),
                ..RenderRequest::default()
            },
        );
        assert!(
            matches!(output, RenderOutput::Line { text, next_frame_ms: Some(value), .. } if text == expected && value == next)
        );
    }
}

#[test]
fn control_text_is_rejected() {
    let root = std::env::temp_dir();
    let paths = Paths {
        config_dir: root.clone(),
        cache_dir: root.clone(),
        data_dir: root.clone(),
    };
    let source = ConfigSource {
        name: "@control.lua".into(),
        source: "local pixy=require('pixy'); return pixy.config({zones={bad=pixy.zone({pixy.segment('value',function() return 'a\\tb' end)})}})".into(),
        directory: root,
        path: None,
    };
    let engine = Engine::load(source, &paths).expect("engine");
    assert!(
        engine
            .render(RenderRequest {
                select: vec!["bad".into()],
                target: Some(LineTarget::Plain),
                ..RenderRequest::default()
            })
            .is_err()
    );
}

#[test]
fn named_zone_segments_are_prioritized_and_interactive() {
    let source = r#"
local pixy=require("pixy")
return pixy.config({zones={
  segments=pixy.zone({
    pixy.segment("a",function() return "a" end,{priority=1}),
    pixy.segment("b",function() return "b" end,{priority=100}),
    pixy.segment("c",function() return "c" end,{priority=2}),
  }),
  status=pixy.zone({
    pixy.segment("left",function() return "A" end,{priority=1}),
    pixy.segment("hidden",function() return "B" end,{priority=100}),
    pixy.segment("center",function() return "C" end,{priority=2}),
    pixy.segment("recording",function() return "R" end,{
        priority=1,
        id="recording",
        actions={left="record.switch",right="record.stop"},
        hover_style={fg=1,bg=2,reverse=true},
        press_styles={left={bg=2,fg=0,bold=true}},
    }),
  }),
}})
"#;
    let segments = render(
        source,
        RenderRequest {
            select: vec!["segments".into()],
            target: Some(LineTarget::Plain),
            width: 2,
            now_ms: Some(0),
            ..RenderRequest::default()
        },
    );
    assert!(matches!(segments, RenderOutput::Line { text, .. } if text == "ac"));

    let mut context = pixy::context::RenderContext::default();
    context
        .values
        .insert("hover_region".into(), serde_json::json!("recording"));
    let status = render(
        source,
        RenderRequest {
            select: vec!["status".into()],
            mode: pixy::RenderMode::Run,
            target: None,
            width: 3,
            now_ms: Some(0),
            context,
            ..RenderRequest::default()
        },
    );
    let RenderOutput::Run {
        runs,
        width,
        regions,
        ..
    } = status
    else {
        panic!("run output");
    };
    assert_eq!(width, 3);
    assert_eq!(
        runs.iter().map(|run| run.text.as_str()).collect::<String>(),
        "ACR"
    );
    assert_eq!(runs.last().expect("recording run").style, "fg:2 bg:1");
    assert_eq!(regions.len(), 1);
    assert_eq!(regions[0].id, "recording");
    assert_eq!((regions[0].x, regions[0].y), (2, 0));
    assert_eq!(regions[0].actions["left"], "record.switch");
    assert_eq!(regions[0].hover_style, "fg:2 bg:1");
    assert_eq!(regions[0].press_styles["left"], "fg:0 bg:2 bold");
}

#[test]
fn interactive_regions_validate_names_and_clip_geometry() {
    let invalid = r#"
local pixy=require("pixy")
return pixy.config({zones={x=pixy.zone({pixy.segment("value",function()
  return pixy.region("x",{id="bad/id",actions={left="record.switch"}})
end)})}})
"#;
    let root = std::env::temp_dir();
    let paths = Paths {
        config_dir: root.clone(),
        cache_dir: root.clone(),
        data_dir: root.clone(),
    };
    let engine = Engine::load(
        ConfigSource {
            name: "@invalid-region.lua".into(),
            source: invalid.into(),
            directory: root,
            path: None,
        },
        &paths,
    )
    .expect("engine");
    assert!(
        engine
            .render(RenderRequest {
                select: vec!["x".into()],
                target: Some(LineTarget::Plain),
                ..RenderRequest::default()
            })
            .expect_err("invalid region")
            .to_string()
            .contains("valid id")
    );

    let clipped = render(
        r#"
local pixy=require("pixy")
return pixy.config({zones={x=pixy.zone({pixy.segment("value",function()
  return pixy.region("abcdef",{id="bounded",actions={left="open"}})
end)})}})
"#,
        RenderRequest {
            select: vec!["x".into()],
            target: Some(LineTarget::Plain),
            width: 3,
            ..RenderRequest::default()
        },
    );
    let RenderOutput::Line { text, regions, .. } = clipped else {
        panic!("line output");
    };
    assert_eq!(text, "abc");
    assert_eq!((regions[0].x, regions[0].width), (0, 3));
}

const PROGRESS_CONFIG: &str = r#"
local pixy = require("pixy")
local progress = require("pixy.segments.progress")
return pixy.config({zones = {
  work = pixy.zone({pixy.segment("value", function(ctx)
    return progress.segment({width = 10}, ctx)
  end)}),
  spin = pixy.zone({pixy.segment("value", function(ctx)
    return progress.spinner("dots", {}, ctx)
  end)}),
}})
"#;

fn progress_line(state: &str, percent: Option<i64>, now_ms: u64) -> (String, Option<u64>) {
    let mut request = RenderRequest {
        select: vec!["work".into()],
        target: Some(LineTarget::Plain),
        width: 40,
        now_ms: Some(now_ms),
        ..RenderRequest::default()
    };
    request
        .context
        .values
        .insert("progress_state".into(), state.into());
    if let Some(percent) = percent {
        request
            .context
            .values
            .insert("progress_pct".into(), percent.into());
    }
    match render(PROGRESS_CONFIG, request) {
        RenderOutput::Line {
            text,
            next_frame_ms,
            ..
        } => (text, next_frame_ms),
        _ => panic!("line output"),
    }
}

#[test]
fn progress_draws_a_bar_for_a_reported_percentage() {
    assert_eq!(progress_line("in_progress", Some(0), 0).0, "░░░░░░░░░░ 0%");
    assert_eq!(
        progress_line("in_progress", Some(50), 0).0,
        "█████░░░░░ 50%"
    );
    assert_eq!(
        progress_line("in_progress", Some(100), 0).0,
        "██████████ 100%"
    );
    // A fraction past half a cell shows a partial glyph, so "started" and
    // "not started yet" never look the same.
    assert_eq!(
        progress_line("in_progress", Some(35), 0).0,
        "███▓░░░░░░ 35%"
    );
    // Out of range is clamped rather than drawn past the end.
    assert_eq!(
        progress_line("in_progress", Some(400), 0).0,
        "██████████ 100%"
    );
}

#[test]
fn progress_reports_nothing_when_the_host_reports_nothing() {
    assert_eq!(progress_line("inactive", Some(40), 0).0, "");
    assert_eq!(progress_line("nonsense", Some(40), 0).0, "");
}

#[test]
fn indeterminate_progress_sweeps_and_asks_to_be_polled() {
    let (first, deadline) = progress_line("indeterminate", None, 100);
    assert_eq!(first, "░███░░░░░░");
    assert_eq!(deadline, Some(180), "the next distinct frame, not a timer");
    let (later, _) = progress_line("indeterminate", None, 360);
    assert_ne!(first, later, "the block moves");
    // A percentage is ignored while the host says it cannot measure progress.
    assert_eq!(progress_line("indeterminate", Some(50), 100).0, first);
}

#[test]
fn named_spinners_advance_and_report_their_next_frame() {
    let frame = |now_ms: u64| {
        let request = RenderRequest {
            select: vec!["spin".into()],
            target: Some(LineTarget::Plain),
            width: 10,
            now_ms: Some(now_ms),
            ..RenderRequest::default()
        };
        match render(PROGRESS_CONFIG, request) {
            RenderOutput::Line {
                text,
                next_frame_ms,
                ..
            } => (text, next_frame_ms),
            _ => panic!("line output"),
        }
    };
    assert_eq!(frame(0).0, "⠋");
    assert_eq!(frame(80).0, "⠙");
    assert_eq!(frame(0).1, Some(80));
}
