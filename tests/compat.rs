use pixy::config::{ConfigSource, Paths};
use pixy::{Engine, LineTarget, RenderMode, RenderOutput, RenderRequest};

fn engine() -> Engine {
    let root = std::env::temp_dir();
    let paths = Paths {
        config_dir: root.clone(),
        cache_dir: root.clone(),
        data_dir: root.clone(),
    };
    Engine::load(
        ConfigSource {
            name: "@examples/hexe-oslo.lua".into(),
            source: include_str!("../examples/hexe-oslo.lua").into(),
            directory: root,
            path: None,
        },
        &paths,
    )
    .expect("compat engine")
}

fn context() -> pixy::context::RenderContext {
    serde_json::from_str(include_str!("fixtures/contexts/hexe-oslo.json")).expect("fixture context")
}

fn line(name: &str, width: u16) -> String {
    let output = engine()
        .render(RenderRequest {
            select: vec![name.into()],
            target: Some(LineTarget::Plain),
            width,
            now_ms: Some(0),
            context: context(),
            ..RenderRequest::default()
        })
        .expect("render line");
    match output {
        RenderOutput::Line { text, .. } => text,
        _ => panic!("line output"),
    }
}

#[test]
fn hexe_oslo_prompt_zones_render_padded_badges() {
    assert_eq!(
        line("prompt.left", 200),
        " //host  nix bresilla ▓| ❄  sudo | 2  7   >> | λ "
    );
    assert_eq!(
        line("prompt.right", 200),
        "| pod | ~d/c/t/pixy  main  !  N "
    );
    assert_eq!(line("prompt.left", 20), " bresilla  7  λ ");
}

#[test]
fn live_hexe_status_zones_and_recording_actions_are_reproduced() {
    let render = |name: &str| {
        engine()
            .render(RenderRequest {
                select: vec![name.into()],
                mode: RenderMode::Run,
                target: None,
                width: 100,
                now_ms: Some(0),
                context: context(),
                ..RenderRequest::default()
            })
            .expect("render status zone")
    };
    let text = |output: &RenderOutput| -> String {
        match output {
            RenderOutput::Run { runs, .. } => runs.iter().map(|run| run.text.as_str()).collect(),
            _ => panic!("run output"),
        }
    };
    assert_eq!(
        text(&render("status.left")),
        " 12:34:56 | session | running build "
    );
    assert_eq!(text(&render("status.center")), " main | logs ");
    let output = render("status.right");
    assert_eq!(text(&output), " REC  87%  ~d/c/t/pixy ");
    let RenderOutput::Run { regions, .. } = output else {
        panic!("run output");
    };
    assert_eq!(regions.len(), 1);
    assert_eq!(regions[0].id, "recording");
    assert_eq!((regions[0].x, regions[0].width), (0, 5));
    assert_eq!(regions[0].actions["left"], "record.switch");
    assert_eq!(regions[0].actions["right"], "record.stop");
    assert_eq!(regions[0].hover_style, "fg:1 bg:15 bold");
}

#[test]
fn live_hexe_knight_rider_spinner_is_reproduced() {
    let mut spinner_context = context();
    spinner_context.values.remove("spinner");
    let render = |now_ms| {
        engine()
            .render(RenderRequest {
                select: vec!["status.left.spinner".into()],
                mode: RenderMode::Run,
                target: None,
                width: 20,
                now_ms: Some(now_ms),
                context: spinner_context.clone(),
                ..RenderRequest::default()
            })
            .expect("render spinner")
    };
    let first = render(0);
    let second = render(40);
    let RenderOutput::Run {
        runs,
        next_frame_ms,
        ..
    } = first
    else {
        panic!("spinner run");
    };
    assert_eq!(
        runs.iter().map(|run| run.text.as_str()).collect::<String>(),
        " ■⬝⬝⬝⬝⬝⬝⬝⬝⬝ "
    );
    assert_eq!(next_frame_ms, Some(40));
    assert_eq!(runs[1].style, "fg:243 bg:0");
    assert_eq!(runs[2].style, "fg:239 bg:0");
    assert!(matches!(
        second,
        RenderOutput::Run {
            next_frame_ms: Some(80),
            ..
        }
    ));
}

#[test]
fn live_hexe_truecolor_sprite_surface_is_reproduced() {
    let mut sprite_context = context();
    sprite_context
        .values
        .insert("sprite_position".into(), serde_json::json!("center"));
    let output = engine()
        .render(RenderRequest {
            select: vec!["overlay.sprite".into()],
            mode: RenderMode::Surface,
            target: None,
            width: 80,
            height: 40,
            now_ms: Some(0),
            context: sprite_context,
            ..RenderRequest::default()
        })
        .expect("render sprite");
    let RenderOutput::Surface {
        ansi,
        width,
        height,
        ..
    } = output
    else {
        panic!("sprite surface");
    };
    assert!(width > 10 && width <= 80);
    assert!(height > 5 && height <= 40);
    assert!(ansi.contains("38;2;"));
    assert!(ansi.contains("48;2;"));
    assert!(ansi.contains('▄') && ansi.contains('▀'));
    assert!(ansi.contains('C'));
}

#[test]
fn live_hexe_frames_popups_and_chooser_are_reproduced() {
    let mut frame_context = context();
    frame_context
        .values
        .insert("title".into(), serde_json::json!("box"));
    frame_context
        .values
        .insert("active".into(), serde_json::json!(true));
    let render_surface =
        |name: &str, width: u16, height: u16, context: pixy::context::RenderContext| {
            engine()
                .render(RenderRequest {
                    select: vec![name.into()],
                    mode: RenderMode::Surface,
                    target: None,
                    width,
                    height,
                    now_ms: Some(0),
                    context,
                    ..RenderRequest::default()
                })
                .expect("render surface")
        };
    let container = render_surface("container.frame", 12, 4, frame_context.clone());
    let RenderOutput::Surface {
        ansi,
        width,
        height,
        ..
    } = container
    else {
        panic!("container surface");
    };
    assert_eq!((width, height), (12, 4));
    assert_eq!(ansi.lines().count(), 4);
    assert!(ansi.lines().next().expect("top").contains("box"));
    assert!(ansi.contains('╔') && ansi.contains('╝'));
    assert!(ansi.contains("48;5;236"));

    let float = render_surface("float.frame", 12, 4, frame_context);
    let RenderOutput::Surface { ansi, .. } = float else {
        panic!("float surface");
    };
    assert!(ansi.lines().last().expect("bottom").contains("box"));

    let mut popup_context = context();
    popup_context
        .values
        .insert("message".into(), serde_json::json!("confirm"));
    let popup = render_surface("pop.confirm", 20, 3, popup_context);
    assert!(
        matches!(popup, RenderOutput::Surface { ansi, height: 3, .. } if ansi.contains("confirm"))
    );

    let mut notify_context = context();
    notify_context
        .values
        .insert("message".into(), serde_json::json!("notice"));
    let notify = render_surface("pop.notify", 20, 3, notify_context);
    assert!(
        matches!(notify, RenderOutput::Surface { ansi, height: 3, .. } if ansi.contains("notice"))
    );

    assert_eq!(line("split.vertical", 1), "│");
    assert_eq!(line("split.horizontal", 1), "─");

    let mut choose_context = context();
    choose_context.values.insert(
        "choices".into(),
        serde_json::json!([
            "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
            "eleven", "twelve"
        ]),
    );
    choose_context
        .values
        .insert("selected".into(), serde_json::json!(12));
    let choose = render_surface("pop.choose", 20, 10, choose_context);
    let RenderOutput::Surface {
        ansi,
        height,
        regions,
        ..
    } = choose
    else {
        panic!("chooser surface");
    };
    assert_eq!(height, 10);
    assert!(!ansi.contains("two"));
    assert!(ansi.contains("three") && ansi.contains("twelve"));
    assert!(ansi.contains("38;5;1;48;5;232"));
    assert_eq!(regions.len(), 10);
    assert_eq!(regions[0].id, "choice.3");
    assert_eq!(regions[9].actions["left"], "choose.12");
}

#[test]
fn live_oslo_direnv_reports_are_reproduced() {
    let render_report = |report: serde_json::Value| {
        let mut report_context = context();
        report_context.values.insert("direnv".into(), report);
        engine()
            .render(RenderRequest {
                select: vec!["oslo.direnv".into()],
                mode: RenderMode::Surface,
                target: None,
                width: 40,
                height: 10,
                now_ms: Some(0),
                context: report_context,
                ..RenderRequest::default()
            })
            .expect("render direnv report")
    };
    let loaded = render_report(serde_json::json!({
        "state": "loaded",
        "owner": "/home/bresilla/project",
        "watched": {"TOP_HEAD": "abc", "PATH": "/one:/two:/three"},
        "changed": [
            {"name": "FOO", "change": "added"},
            {"name": "BAR", "change": "removed"}
        ],
        "aliases": [{"name": "ll"}]
    }));
    let RenderOutput::Surface { ansi, height, .. } = loaded else {
        panic!("loaded report surface");
    };
    assert_eq!(height, 6);
    for text in [
        "direnv",
        "loaded",
        "~/project",
        "TOP_HEAD",
        "PATH",
        "FOO",
        "BAR",
        "ll",
    ] {
        assert!(ansi.contains(text), "missing {text:?}");
    }

    let blocked = render_report(serde_json::json!({
        "state": "blocked",
        "owner": "/home/bresilla/project"
    }));
    assert!(
        matches!(blocked, RenderOutput::Surface { ansi, height: 1, .. } if ansi.contains("blocked") && ansi.contains("direnv allow"))
    );
}

#[test]
fn live_tab_and_pane_activity_follow_the_host_fields() {
    let styled = |select: &str, values: &str| -> Vec<(String, String)> {
        let context = serde_json::from_str(values).expect("context values");
        let output = engine()
            .render(RenderRequest {
                select: vec![select.into()],
                mode: RenderMode::Run,
                target: None,
                width: 40,
                now_ms: Some(0),
                context,
                ..RenderRequest::default()
            })
            .expect("render view");
        match output {
            RenderOutput::Run { runs, .. } => {
                runs.into_iter().map(|run| (run.text, run.style)).collect()
            }
            _ => panic!("run output"),
        }
    };
    let active = "fg:0 bg:1";
    let idle = "fg:250 bg:237";
    let tabs = |index: u8| {
        styled(
            "status.center",
            &format!("{{\"values\":{{\"tabs\":[\"main\",\"logs\"],\"active_tab\":{index}}}}}"),
        )
    };
    assert_eq!(
        tabs(0)[0].1,
        active,
        "a zero-based active_tab marks the first tab"
    );
    assert_eq!(tabs(0)[2].1, idle);
    assert_eq!(tabs(1)[0].1, idle);
    assert_eq!(tabs(1)[2].1, active);
    let title = |flag: bool| {
        styled(
            "container.title",
            &format!("{{\"values\":{{\"title\":\"editor\",\"active\":{flag}}}}}"),
        )
    };
    assert_eq!(title(true)[1].1, active);
    assert_eq!(title(false)[1].1, idle);
}
