use pixy::config::{ConfigSource, Paths};
use std::fs;
use std::time::{SystemTime, UNIX_EPOCH};

#[test]
fn xdg_and_pixy_path_precedence_is_exact() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .expect("time")
        .as_nanos();
    let root = std::env::temp_dir().join(format!("pixy-paths-{}-{unique}", std::process::id()));
    let home = root.join("home");
    let xdg_config = root.join("xdg-config");
    let xdg_cache = root.join("xdg-cache");
    let xdg_data = root.join("xdg-data");
    for path in [&home, &xdg_config, &xdg_cache, &xdg_data] {
        fs::create_dir_all(path).expect("path");
    }
    unsafe {
        std::env::set_var("HOME", &home);
        std::env::set_var("XDG_CONFIG_HOME", &xdg_config);
        std::env::set_var("XDG_CACHE_HOME", &xdg_cache);
        std::env::set_var("XDG_DATA_HOME", &xdg_data);
        std::env::remove_var("PIXY_CONFIG");
        std::env::remove_var("PIXY_CACHE_DIR");
        std::env::remove_var("PIXY_DATA_DIR");
    }
    let paths = Paths::discover().expect("paths");
    assert_eq!(paths.config_dir, xdg_config.join("pixy"));
    assert_eq!(paths.cache_dir, xdg_cache.join("pixy"));
    assert_eq!(paths.data_dir, xdg_data.join("pixy/packs"));

    unsafe { std::env::remove_var("HOME") };
    let paths = Paths::discover().expect("XDG paths without HOME");
    assert_eq!(paths.config_dir, xdg_config.join("pixy"));
    assert_eq!(paths.cache_dir, xdg_cache.join("pixy"));
    assert_eq!(paths.data_dir, xdg_data.join("pixy/packs"));

    let overrides = root.join("overrides");
    unsafe {
        std::env::set_var("PIXY_CACHE_DIR", overrides.join("cache"));
        std::env::set_var("PIXY_DATA_DIR", overrides.join("data"));
    }
    let paths = Paths::discover().expect("override paths");
    assert_eq!(paths.cache_dir, overrides.join("cache"));
    assert_eq!(paths.data_dir, overrides.join("data"));

    let environment_config = root.join("environment.lua");
    let explicit_config = root.join("explicit.lua");
    fs::write(&environment_config, "return {zones={}}").expect("environment config");
    fs::write(&explicit_config, "return {zones={}}").expect("explicit config");
    unsafe { std::env::set_var("PIXY_CONFIG", "") };
    assert_eq!(
        ConfigSource::load(None, &paths)
            .expect("empty override")
            .path,
        None
    );
    unsafe { std::env::set_var("PIXY_CONFIG", &environment_config) };
    assert_eq!(
        ConfigSource::load(None, &paths)
            .expect("environment source")
            .path,
        Some(environment_config)
    );
    assert_eq!(
        ConfigSource::load(Some(&explicit_config), &paths)
            .expect("explicit source")
            .path,
        Some(explicit_config)
    );
    fs::remove_dir_all(root).expect("cleanup");
}
