use crate::model::error::{PixyError, Result};
use flate2::read::GzDecoder;
use std::io::Read;

const PACK: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/pokemon.pack"));
const MAGIC: &[u8; 4] = b"HXSP";
const HEADER_SIZE: usize = 16;
const MAX_SPRITES: usize = 4096;
const MAX_SPRITE_SIZE: usize = 1024 * 1024;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct EmbeddedPack {
    pub name: &'static str,
    pub items: usize,
    pub source: &'static str,
    pub license: &'static str,
    pub attribution: &'static str,
}

pub fn packs() -> Result<Vec<EmbeddedPack>> {
    let (_, count, _) = header()?;
    Ok(vec![EmbeddedPack {
        name: "pokemon",
        items: count,
        source: "krabby / PokéSprite via Hexe 3ddc19b",
        license: "GPL-3.0-only; sprite images © Nintendo/Creatures Inc./GAME FREAK Inc.",
        attribution: "yannjor/krabby, msikma/PokéSprite, pokemon-generator-scripts, PokéAPI",
    }])
}

pub fn item(pack: &str, name: &str) -> Result<Option<Vec<u8>>> {
    if pack != "pokemon" {
        return Ok(None);
    }
    let (kind, wanted) = if let Some(name) = name.strip_prefix("regular/") {
        (0, name)
    } else if let Some(name) = name.strip_prefix("shiny/") {
        (1, name)
    } else {
        return Ok(None);
    };
    if wanted.is_empty() || wanted.contains('/') {
        return Ok(None);
    }
    locate(kind, wanted)
}

fn locate(wanted_kind: u8, wanted_name: &str) -> Result<Option<Vec<u8>>> {
    let (data_start, count, data_len) = header()?;
    let mut cursor = HEADER_SIZE;
    let mut data_offset = 0_usize;
    for _ in 0..count {
        if cursor.checked_add(10).is_none_or(|end| end > data_start) {
            return Err(corrupt());
        }
        let kind = PACK[cursor];
        let name_len = usize::from(PACK[cursor + 1]);
        let raw_len = read_u32(cursor + 2)? as usize;
        let compressed_len = read_u32(cursor + 6)? as usize;
        cursor += 10;
        let name_end = cursor.checked_add(name_len).ok_or_else(corrupt)?;
        if name_end > data_start || raw_len > MAX_SPRITE_SIZE {
            return Err(corrupt());
        }
        let name = std::str::from_utf8(&PACK[cursor..name_end]).map_err(|_| corrupt())?;
        cursor = name_end;
        let compressed_end = data_offset
            .checked_add(compressed_len)
            .ok_or_else(corrupt)?;
        if compressed_end > data_len {
            return Err(corrupt());
        }
        if kind == wanted_kind && name == wanted_name {
            let start = data_start + data_offset;
            return inflate(&PACK[start..start + compressed_len], raw_len).map(Some);
        }
        data_offset = compressed_end;
    }
    if cursor != data_start || data_offset != data_len {
        return Err(corrupt());
    }
    Ok(None)
}

fn header() -> Result<(usize, usize, usize)> {
    if PACK.len() < HEADER_SIZE || &PACK[..MAGIC.len()] != MAGIC || read_u32(4)? != 1 {
        return Err(corrupt());
    }
    let count = read_u32(8)? as usize;
    let index_len = read_u32(12)? as usize;
    let data_start = HEADER_SIZE.checked_add(index_len).ok_or_else(corrupt)?;
    if count == 0 || count > MAX_SPRITES || data_start > PACK.len() {
        return Err(corrupt());
    }
    Ok((data_start, count, PACK.len() - data_start))
}

fn inflate(compressed: &[u8], raw_len: usize) -> Result<Vec<u8>> {
    let mut output = Vec::with_capacity(raw_len);
    GzDecoder::new(compressed)
        .take((raw_len + 1) as u64)
        .read_to_end(&mut output)
        .map_err(|_| corrupt())?;
    if output.len() != raw_len {
        return Err(corrupt());
    }
    Ok(output)
}

fn read_u32(offset: usize) -> Result<u32> {
    let bytes = PACK.get(offset..offset + 4).ok_or_else(corrupt)?;
    Ok(u32::from_le_bytes(bytes.try_into().expect("four bytes")))
}

fn corrupt() -> PixyError {
    PixyError::Transport("corrupt embedded Pokemon archive".into())
}
