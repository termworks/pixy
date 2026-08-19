use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::BTreeMap;

/// Everything a caller hands a render, as two open maps.
///
/// Rust interprets none of it: `env` is read back by `pixy.host.env` and
/// `values` reaches Lua as `ctx.values`. What a prompt or a statusbar is made
/// of is named by the configuration's zones, never by a field here.
#[derive(Clone, Debug, Default, Deserialize, Serialize)]
pub struct RenderContext {
    #[serde(default)]
    pub env: BTreeMap<String, Option<String>>,
    #[serde(default)]
    pub values: BTreeMap<String, Value>,
}
