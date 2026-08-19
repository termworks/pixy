use crate::model::error::{PixyError, Result};
use std::io::Read;
use std::path::{Path, PathBuf};

pub const DEFAULT_CONFIG: &str = include_str!("../../lua/pixy/default.lua");
pub(crate) const MAX_CONFIG_SIZE: usize = 1024 * 1024;

#[derive(Clone, Debug)]
pub struct Paths {
    pub config_dir: PathBuf,
    pub cache_dir: PathBuf,
    pub data_dir: PathBuf,
}

impl Paths {
    pub fn discover() -> Result<Self> {
        let home = environment_path("HOME");
        let config_dir = path_base("XDG_CONFIG_HOME", home.as_deref(), ".config")?.join("pixy");
        let cache_dir = environment_path("PIXY_CACHE_DIR")
            .map(Ok)
            .unwrap_or_else(|| {
                path_base("XDG_CACHE_HOME", home.as_deref(), ".cache").map(|path| path.join("pixy"))
            })?;
        let data_dir = environment_path("PIXY_DATA_DIR")
            .map(Ok)
            .unwrap_or_else(|| {
                path_base("XDG_DATA_HOME", home.as_deref(), ".local/share")
                    .map(|path| path.join("pixy/packs"))
            })?;
        Ok(Self {
            config_dir,
            cache_dir,
            data_dir,
        })
    }

    pub fn default_config(&self) -> PathBuf {
        self.config_dir.join("init.lua")
    }
}

fn path_base(variable: &str, home: Option<&Path>, fallback: &str) -> Result<PathBuf> {
    environment_path(variable)
        .or_else(|| home.map(|path| path.join(fallback)))
        .ok_or_else(|| {
            PixyError::Config(format!(
                "HOME and {variable} are unset, so Pixy paths cannot be resolved"
            ))
        })
}

fn environment_path(variable: &str) -> Option<PathBuf> {
    std::env::var_os(variable)
        .filter(|value| !value.is_empty())
        .map(PathBuf::from)
}

#[derive(Clone, Debug)]
pub struct ConfigSource {
    pub name: String,
    pub source: String,
    pub directory: PathBuf,
    pub path: Option<PathBuf>,
}

impl ConfigSource {
    pub fn load(explicit: Option<&Path>, paths: &Paths) -> Result<Self> {
        let env_path = environment_path("PIXY_CONFIG");
        let selected = explicit.map(PathBuf::from).or(env_path);
        if let Some(path) = selected {
            return read_config(&path, true, paths);
        }
        let path = paths.default_config();
        if path.exists() {
            return read_config(&path, false, paths);
        }
        Ok(Self {
            name: "@pixy/default.lua".into(),
            source: DEFAULT_CONFIG.into(),
            directory: paths.config_dir.clone(),
            path: None,
        })
    }
}

fn read_config(path: &Path, explicit: bool, paths: &Paths) -> Result<ConfigSource> {
    let mut bytes = Vec::new();
    std::fs::File::open(path)
        .and_then(|file| {
            file.take((MAX_CONFIG_SIZE + 1) as u64)
                .read_to_end(&mut bytes)
        })
        .map_err(|error| {
            let kind = if explicit {
                "explicit config"
            } else {
                "config"
            };
            PixyError::Config(format!("failed to read {kind} {}: {error}", path.display()))
        })?;
    if bytes.len() > MAX_CONFIG_SIZE {
        return Err(PixyError::Config(format!(
            "config {} exceeds 1 MiB",
            path.display()
        )));
    }
    let source = String::from_utf8(bytes)
        .map_err(|_| PixyError::Config(format!("config {} is not UTF-8", path.display())))?;
    Ok(ConfigSource {
        name: format!("@{}", path.display()),
        source,
        directory: path.parent().unwrap_or(&paths.config_dir).to_path_buf(),
        path: Some(path.to_path_buf()),
    })
}
