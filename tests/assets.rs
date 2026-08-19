use std::fs;
use std::time::{SystemTime, UNIX_EPOCH};

#[test]
fn embedded_pokemon_pack_contains_regular_and_shiny_sprites() {
    let packs = pixy::assets::embedded_packs().expect("embedded packs");
    assert_eq!(packs.len(), 1);
    assert_eq!(packs[0].name, "pokemon");
    assert_eq!(packs[0].items, 2034);
    for name in ["regular/pikachu", "shiny/pikachu"] {
        let sprite = pixy::assets::embedded_item("pokemon", name)
            .expect("embedded item")
            .expect("Pikachu");
        assert!(sprite.contains(&0x1b));
        assert!(
            String::from_utf8(sprite)
                .expect("ANSI sprite")
                .contains('▀')
        );
    }
    assert_eq!(
        pixy::assets::embedded_item("pokemon", "regular/not-a-pokemon").expect("missing item"),
        None
    );
    assert_eq!(
        pixy::assets::embedded_item("other", "regular/pikachu").expect("other pack"),
        None
    );
}

#[test]
fn pack_build_check_and_item_are_deterministic() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .expect("time")
        .as_nanos();
    let root = std::env::temp_dir().join(format!("pixy-assets-{}-{unique}", std::process::id()));
    let input = root.join("input");
    fs::create_dir_all(&input).expect("directory");
    fs::write(input.join("cat.txt"), "cat").expect("asset");
    fs::write(input.join("dog.txt"), "dog").expect("asset");
    fs::create_dir(input.join("regular")).expect("nested directory");
    fs::write(
        input.join("regular/pikachu"),
        "\u{1b}[38;2;1;2;3m▀\u{1b}[0m",
    )
    .expect("nested asset");
    let first = root.join("first.pixypack");
    let second = root.join("second.pixypack");
    pixy::assets::build(&input, &first, "test".into(), "MIT".into(), "Pixy".into()).expect("build");
    pixy::assets::build(&input, &second, "test".into(), "MIT".into(), "Pixy".into())
        .expect("build");
    assert_eq!(
        fs::read(&first).expect("first"),
        fs::read(&second).expect("second")
    );
    assert!(
        fs::read(&first)
            .expect("binary pack")
            .starts_with(b"PIXYPK2\0")
    );
    assert_eq!(
        pixy::assets::item(&first, "cat.txt").expect("item"),
        Some(b"cat".to_vec())
    );
    assert_eq!(
        pixy::assets::item(&first, "regular/pikachu").expect("nested item"),
        Some("\u{1b}[38;2;1;2;3m▀\u{1b}[0m".as_bytes().to_vec())
    );
    let mut corrupt = pixy::assets::load(&first).expect("pack");
    corrupt.items.get_mut("dog.txt").expect("asset").checksum = "fnv1a64:0000000000000000".into();
    fs::write(&first, serde_json::to_vec(&corrupt).expect("json")).expect("corrupt pack");
    assert_eq!(
        pixy::assets::item(&first, "cat.txt").expect("selected item"),
        Some(b"cat".to_vec())
    );
    assert!(pixy::assets::load(&first).is_err());
    let mut oversized = pixy::assets::load(&second).expect("pack");
    oversized.items.get_mut("cat.txt").expect("asset").raw_size = 1024 * 1024 + 1;
    fs::write(&second, serde_json::to_vec(&oversized).expect("json")).expect("oversized pack");
    assert!(pixy::assets::item(&second, "cat.txt").is_err());
    fs::remove_dir_all(root).expect("cleanup");
}
