use crate::model::error::{PixyError, Result};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::io::{Read, Write};
use std::path::{Component, Path, PathBuf};

mod embedded;

pub use embedded::{
    EmbeddedPack, item as embedded_item, item_names as embedded_item_names, packs as embedded_packs,
};

const MAX_PACK_SIZE: usize = 16 * 1024 * 1024;
const MAX_ITEMS: usize = 4096;
const MAX_ITEM_SIZE: usize = 1024 * 1024;
const MAX_TOTAL_RAW_SIZE: usize = 64 * 1024 * 1024;
const MAX_NAME_SIZE: usize = 255;
const MAX_METADATA_SIZE: usize = 4096;
const BINARY_MAGIC: &[u8; 8] = b"PIXYPK2\0";
const BINARY_HEADER_SIZE: usize = 60;

struct BuildItem {
    name: String,
    raw_size: usize,
    checksum: u64,
    stored: Vec<u8>,
}

struct BinaryEntry<'a> {
    name: &'a str,
    raw_size: usize,
    stored_size: usize,
    checksum: u64,
    offset: usize,
}

struct BinaryIndex<'a> {
    source: &'a str,
    license: &'a str,
    attribution: &'a str,
    content_hash: u64,
    entries: Vec<BinaryEntry<'a>>,
    data: &'a [u8],
}

#[derive(Debug, Deserialize, Serialize)]
pub struct Pack {
    pub version: u16,
    pub source: String,
    pub license: String,
    pub attribution: String,
    pub content_hash: String,
    pub items: BTreeMap<String, PackItem>,
}

#[derive(Debug, Deserialize, Serialize)]
pub struct PackItem {
    pub codec: String,
    pub raw_size: usize,
    pub stored_size: usize,
    pub checksum: String,
    pub data_hex: String,
}

pub fn build(
    directory: &Path,
    output: &Path,
    source: String,
    license: String,
    attribution: String,
) -> Result<()> {
    if source.len() > MAX_METADATA_SIZE
        || license.len() > MAX_METADATA_SIZE
        || attribution.len() > MAX_METADATA_SIZE
    {
        return Err(PixyError::Usage("pack metadata exceeds size limit".into()));
    }
    let mut paths = Vec::new();
    collect_files(directory, directory, &mut paths)?;
    paths.sort_by(|left, right| left.0.cmp(&right.0));
    let mut items = Vec::with_capacity(paths.len());
    if paths.len() > MAX_ITEMS {
        return Err(PixyError::Usage(format!(
            "pack exceeds {MAX_ITEMS} item limit"
        )));
    }
    let mut content = Fnv1a::new();
    let mut total_raw_size = 0_usize;
    for (name, path) in paths {
        validate_name(&name)?;
        let bytes = std::fs::read(&path).map_err(asset_error)?;
        if bytes.len() > MAX_ITEM_SIZE {
            return Err(PixyError::Usage(format!("asset {name} exceeds 1 MiB")));
        }
        total_raw_size = total_raw_size.saturating_add(bytes.len());
        if total_raw_size > MAX_TOTAL_RAW_SIZE {
            return Err(PixyError::Usage(
                "pack exceeds 64 MiB raw size limit".into(),
            ));
        }
        let stored = compress(&bytes)?;
        content.update(name.as_bytes());
        content.update(&(stored.len() as u64).to_be_bytes());
        content.update(&stored);
        items.push(BuildItem {
            name,
            raw_size: bytes.len(),
            checksum: hash_value(&bytes),
            stored,
        });
    }
    let mut index = Vec::new();
    let mut offset = 0_u64;
    for item in &items {
        append_u16(&mut index, item.name.len() as u16);
        append_u32(&mut index, item.raw_size as u32);
        append_u32(&mut index, item.stored.len() as u32);
        append_u64(&mut index, item.checksum);
        append_u64(&mut index, offset);
        index.extend_from_slice(item.name.as_bytes());
        offset += item.stored.len() as u64;
    }
    let mut index_hash = Fnv1a::new();
    index_hash.update(&index);
    let mut bytes = Vec::with_capacity(
        BINARY_HEADER_SIZE
            + source.len()
            + license.len()
            + attribution.len()
            + index.len()
            + offset as usize,
    );
    bytes.extend_from_slice(BINARY_MAGIC);
    append_u32(&mut bytes, 2);
    append_u32(&mut bytes, items.len() as u32);
    append_u32(&mut bytes, source.len() as u32);
    append_u32(&mut bytes, license.len() as u32);
    append_u32(&mut bytes, attribution.len() as u32);
    append_u64(&mut bytes, index.len() as u64);
    append_u64(&mut bytes, offset);
    append_u64(&mut bytes, index_hash.value());
    append_u64(&mut bytes, content.value());
    bytes.extend_from_slice(source.as_bytes());
    bytes.extend_from_slice(license.as_bytes());
    bytes.extend_from_slice(attribution.as_bytes());
    bytes.extend_from_slice(&index);
    for item in items {
        bytes.extend_from_slice(&item.stored);
    }
    if bytes.len() > MAX_PACK_SIZE {
        return Err(PixyError::Usage("pack exceeds 16 MiB".into()));
    }
    let temporary = output.with_extension(format!("pixypack.tmp-{}", std::process::id()));
    std::fs::write(&temporary, bytes).map_err(asset_error)?;
    if let Err(error) = std::fs::rename(&temporary, output) {
        let _ = std::fs::remove_file(&temporary);
        return Err(asset_error(error));
    }
    Ok(())
}

fn collect_files(root: &Path, directory: &Path, output: &mut Vec<(String, PathBuf)>) -> Result<()> {
    let mut entries = std::fs::read_dir(directory)
        .map_err(asset_error)?
        .collect::<std::io::Result<Vec<_>>>()
        .map_err(asset_error)?;
    entries.sort_by_key(std::fs::DirEntry::file_name);
    for entry in entries {
        let kind = entry.file_type().map_err(asset_error)?;
        let path = entry.path();
        if kind.is_symlink() {
            return Err(PixyError::Usage(format!(
                "asset source contains symlink {}",
                path.display()
            )));
        }
        if kind.is_dir() {
            collect_files(root, &path, output)?;
        } else if kind.is_file() {
            let relative = path.strip_prefix(root).map_err(|error| {
                PixyError::Usage(format!("invalid asset path {}: {error}", path.display()))
            })?;
            let name = relative
                .components()
                .map(|part| match part {
                    Component::Normal(value) => value
                        .to_str()
                        .map(str::to_string)
                        .ok_or_else(|| PixyError::Usage("asset name is not UTF-8".into())),
                    _ => Err(PixyError::Usage("invalid asset path".into())),
                })
                .collect::<Result<Vec<_>>>()?
                .join("/");
            output.push((name, path));
        }
    }
    Ok(())
}

pub fn load(path: &Path) -> Result<Pack> {
    let pack = parse(path)?;
    check(&pack)?;
    Ok(pack)
}

pub fn check(pack: &Pack) -> Result<()> {
    check_index(pack)?;
    for (name, item) in &pack.items {
        let stored = unhex(&item.data_hex)?;
        let bytes = decompress(&stored, item.raw_size)?;
        if bytes.len() != item.raw_size || hash(&bytes) != item.checksum {
            return Err(PixyError::Transport(format!("corrupt asset {name}")));
        }
    }
    Ok(())
}

fn check_index(pack: &Pack) -> Result<()> {
    if !matches!(pack.version, 1 | 2) {
        return Err(PixyError::Transport(format!(
            "unsupported pack version {}",
            pack.version
        )));
    }
    if pack.items.len() > MAX_ITEMS {
        return Err(PixyError::Transport(format!(
            "pack exceeds {MAX_ITEMS} item limit"
        )));
    }
    if pack.source.len() > MAX_METADATA_SIZE
        || pack.license.len() > MAX_METADATA_SIZE
        || pack.attribution.len() > MAX_METADATA_SIZE
    {
        return Err(PixyError::Transport(
            "pack metadata exceeds size limit".into(),
        ));
    }
    let mut content = Fnv1a::new();
    let mut total_raw_size = 0_usize;
    for (name, item) in &pack.items {
        validate_name(name)?;
        if item.codec != "deflate" {
            return Err(PixyError::Transport(format!(
                "unsupported codec {}",
                item.codec
            )));
        }
        if item.raw_size > MAX_ITEM_SIZE {
            return Err(PixyError::Transport(format!(
                "asset {name} exceeds raw size cap"
            )));
        }
        if item.stored_size > MAX_ITEM_SIZE + 64 * 1024 {
            return Err(PixyError::Transport(format!(
                "stored asset {name} exceeds size cap"
            )));
        }
        if item.data_hex.len() != item.stored_size.saturating_mul(2) {
            return Err(PixyError::Transport(format!("corrupt asset {name}")));
        }
        let stored = unhex(&item.data_hex)?;
        total_raw_size = total_raw_size.saturating_add(item.raw_size);
        if total_raw_size > MAX_TOTAL_RAW_SIZE {
            return Err(PixyError::Transport(
                "pack exceeds 64 MiB raw size limit".into(),
            ));
        }
        content.update(name.as_bytes());
        content.update(&(stored.len() as u64).to_be_bytes());
        content.update(&stored);
    }
    if content.finish() != pack.content_hash {
        return Err(PixyError::Transport("pack content hash mismatch".into()));
    }
    Ok(())
}

pub fn item(path: &Path, name: &str) -> Result<Option<Vec<u8>>> {
    let bytes = read_pack(path)?;
    if bytes.starts_with(BINARY_MAGIC) {
        let index = parse_binary_index(&bytes)?;
        let Some(item) = index.entries.iter().find(|item| item.name == name) else {
            return Ok(None);
        };
        let stored = &index.data[item.offset..item.offset + item.stored_size];
        let bytes = decompress(stored, item.raw_size)?;
        if bytes.len() != item.raw_size || hash_value(&bytes) != item.checksum {
            return Err(PixyError::Transport(format!("corrupt asset {name}")));
        }
        return Ok(Some(bytes));
    }
    let pack = parse_json(&bytes)?;
    check_index(&pack)?;
    let Some(item) = pack.items.get(name) else {
        return Ok(None);
    };
    let stored = unhex(&item.data_hex)?;
    let bytes = decompress(&stored, item.raw_size)?;
    if bytes.len() != item.raw_size || hash(&bytes) != item.checksum {
        return Err(PixyError::Transport(format!("corrupt asset {name}")));
    }
    Ok(Some(bytes))
}

fn parse(path: &Path) -> Result<Pack> {
    let bytes = read_pack(path)?;
    if bytes.starts_with(BINARY_MAGIC) {
        return parse_binary(&bytes);
    }
    parse_json(&bytes)
}

fn read_pack(path: &Path) -> Result<Vec<u8>> {
    let bytes = std::fs::read(path).map_err(asset_error)?;
    if bytes.len() > MAX_PACK_SIZE {
        return Err(PixyError::Transport("pack exceeds 16 MiB".into()));
    }
    Ok(bytes)
}

fn parse_json(bytes: &[u8]) -> Result<Pack> {
    serde_json::from_slice(bytes)
        .map_err(|error| PixyError::Transport(format!("invalid pack: {error}")))
}

fn parse_binary(bytes: &[u8]) -> Result<Pack> {
    let index = parse_binary_index(bytes)?;
    let mut items = BTreeMap::new();
    for entry in index.entries {
        let stored = &index.data[entry.offset..entry.offset + entry.stored_size];
        items.insert(
            entry.name.to_string(),
            PackItem {
                codec: "deflate".into(),
                raw_size: entry.raw_size,
                stored_size: entry.stored_size,
                checksum: format_hash(entry.checksum),
                data_hex: hex(stored),
            },
        );
    }
    Ok(Pack {
        version: 2,
        source: index.source.to_string(),
        license: index.license.to_string(),
        attribution: index.attribution.to_string(),
        content_hash: format_hash(index.content_hash),
        items,
    })
}

fn parse_binary_index(bytes: &[u8]) -> Result<BinaryIndex<'_>> {
    if bytes.len() < BINARY_HEADER_SIZE || !bytes.starts_with(BINARY_MAGIC) {
        return Err(PixyError::Transport("invalid binary pack header".into()));
    }
    let mut cursor = BINARY_MAGIC.len();
    let version = take_u32(bytes, &mut cursor)?;
    if version != 2 {
        return Err(PixyError::Transport(format!(
            "unsupported pack version {version}"
        )));
    }
    let count = take_u32(bytes, &mut cursor)? as usize;
    let source_len = take_u32(bytes, &mut cursor)? as usize;
    let license_len = take_u32(bytes, &mut cursor)? as usize;
    let attribution_len = take_u32(bytes, &mut cursor)? as usize;
    let index_len = usize::try_from(take_u64(bytes, &mut cursor)?)
        .map_err(|_| PixyError::Transport("pack index is too large".into()))?;
    let data_len = usize::try_from(take_u64(bytes, &mut cursor)?)
        .map_err(|_| PixyError::Transport("pack data is too large".into()))?;
    let expected_index_hash = take_u64(bytes, &mut cursor)?;
    let content_hash = take_u64(bytes, &mut cursor)?;
    if count > MAX_ITEMS
        || source_len > MAX_METADATA_SIZE
        || license_len > MAX_METADATA_SIZE
        || attribution_len > MAX_METADATA_SIZE
    {
        return Err(PixyError::Transport("pack index exceeds limits".into()));
    }
    let source = take_utf8(bytes, &mut cursor, source_len, "source")?;
    let license = take_utf8(bytes, &mut cursor, license_len, "license")?;
    let attribution = take_utf8(bytes, &mut cursor, attribution_len, "attribution")?;
    let index_end = cursor
        .checked_add(index_len)
        .ok_or_else(|| PixyError::Transport("pack index overflow".into()))?;
    let data_end = index_end
        .checked_add(data_len)
        .ok_or_else(|| PixyError::Transport("pack data overflow".into()))?;
    if data_end != bytes.len() {
        return Err(PixyError::Transport("pack size mismatch".into()));
    }
    let mut index_hash = Fnv1a::new();
    index_hash.update(&bytes[cursor..index_end]);
    if index_hash.value() != expected_index_hash {
        return Err(PixyError::Transport("pack index hash mismatch".into()));
    }
    let mut entries = Vec::with_capacity(count);
    let mut previous = None;
    let mut expected_offset = 0_usize;
    let mut total_raw_size = 0_usize;
    for _ in 0..count {
        let name_len = take_u16_at(bytes, &mut cursor, index_end)? as usize;
        let raw_size = take_u32_at(bytes, &mut cursor, index_end)? as usize;
        let stored_size = take_u32_at(bytes, &mut cursor, index_end)? as usize;
        let checksum = take_u64_at(bytes, &mut cursor, index_end)?;
        let offset = usize::try_from(take_u64_at(bytes, &mut cursor, index_end)?)
            .map_err(|_| PixyError::Transport("asset offset is too large".into()))?;
        let name = take_utf8_at(bytes, &mut cursor, name_len, index_end, "asset name")?;
        validate_name(name)?;
        if previous.is_some_and(|value| value >= name) {
            return Err(PixyError::Transport("asset index is not sorted".into()));
        }
        if raw_size > MAX_ITEM_SIZE || stored_size > MAX_ITEM_SIZE + 64 * 1024 {
            return Err(PixyError::Transport(format!(
                "asset {name} exceeds size cap"
            )));
        }
        total_raw_size = total_raw_size.saturating_add(raw_size);
        if total_raw_size > MAX_TOTAL_RAW_SIZE || offset != expected_offset {
            return Err(PixyError::Transport("invalid asset index bounds".into()));
        }
        expected_offset = expected_offset
            .checked_add(stored_size)
            .ok_or_else(|| PixyError::Transport("asset offset overflow".into()))?;
        if expected_offset > data_len {
            return Err(PixyError::Transport("asset exceeds pack data".into()));
        }
        entries.push(BinaryEntry {
            name,
            raw_size,
            stored_size,
            checksum,
            offset,
        });
        previous = Some(name);
    }
    if cursor != index_end || expected_offset != data_len {
        return Err(PixyError::Transport("pack index length mismatch".into()));
    }
    Ok(BinaryIndex {
        source,
        license,
        attribution,
        content_hash,
        entries,
        data: &bytes[index_end..data_end],
    })
}

fn append_u16(output: &mut Vec<u8>, value: u16) {
    output.extend_from_slice(&value.to_le_bytes());
}

fn append_u32(output: &mut Vec<u8>, value: u32) {
    output.extend_from_slice(&value.to_le_bytes());
}

fn append_u64(output: &mut Vec<u8>, value: u64) {
    output.extend_from_slice(&value.to_le_bytes());
}

fn take_u32(bytes: &[u8], cursor: &mut usize) -> Result<u32> {
    let value = take(bytes, cursor, 4, bytes.len())?;
    Ok(u32::from_le_bytes(value.try_into().expect("four bytes")))
}

fn take_u64(bytes: &[u8], cursor: &mut usize) -> Result<u64> {
    let value = take(bytes, cursor, 8, bytes.len())?;
    Ok(u64::from_le_bytes(value.try_into().expect("eight bytes")))
}

fn take_u16_at(bytes: &[u8], cursor: &mut usize, limit: usize) -> Result<u16> {
    let value = take(bytes, cursor, 2, limit)?;
    Ok(u16::from_le_bytes(value.try_into().expect("two bytes")))
}

fn take_u32_at(bytes: &[u8], cursor: &mut usize, limit: usize) -> Result<u32> {
    let value = take(bytes, cursor, 4, limit)?;
    Ok(u32::from_le_bytes(value.try_into().expect("four bytes")))
}

fn take_u64_at(bytes: &[u8], cursor: &mut usize, limit: usize) -> Result<u64> {
    let value = take(bytes, cursor, 8, limit)?;
    Ok(u64::from_le_bytes(value.try_into().expect("eight bytes")))
}

fn take_utf8<'a>(
    bytes: &'a [u8],
    cursor: &mut usize,
    length: usize,
    field: &str,
) -> Result<&'a str> {
    take_utf8_at(bytes, cursor, length, bytes.len(), field)
}

fn take_utf8_at<'a>(
    bytes: &'a [u8],
    cursor: &mut usize,
    length: usize,
    limit: usize,
    field: &str,
) -> Result<&'a str> {
    std::str::from_utf8(take(bytes, cursor, length, limit)?)
        .map_err(|_| PixyError::Transport(format!("pack {field} is not UTF-8")))
}

fn take<'a>(bytes: &'a [u8], cursor: &mut usize, length: usize, limit: usize) -> Result<&'a [u8]> {
    let end = cursor
        .checked_add(length)
        .ok_or_else(|| PixyError::Transport("pack offset overflow".into()))?;
    if end > limit || end > bytes.len() {
        return Err(PixyError::Transport("truncated pack".into()));
    }
    let value = &bytes[*cursor..end];
    *cursor = end;
    Ok(value)
}

fn validate_name(name: &str) -> Result<()> {
    let path = Path::new(name);
    if name.is_empty()
        || name.len() > MAX_NAME_SIZE
        || path.is_absolute()
        || path
            .components()
            .any(|part| !matches!(part, Component::Normal(_)))
    {
        return Err(PixyError::Usage(format!("invalid asset name {name:?}")));
    }
    Ok(())
}

fn hash(bytes: &[u8]) -> String {
    format_hash(hash_value(bytes))
}

fn hash_value(bytes: &[u8]) -> u64 {
    let mut hash = Fnv1a::new();
    hash.update(bytes);
    hash.value()
}

fn format_hash(value: u64) -> String {
    format!("fnv1a64:{value:016x}")
}

struct Fnv1a(u64);

impl Fnv1a {
    fn new() -> Self {
        Self(0xcbf29ce484222325)
    }

    fn update(&mut self, bytes: &[u8]) {
        for byte in bytes {
            self.0 ^= u64::from(*byte);
            self.0 = self.0.wrapping_mul(0x100000001b3);
        }
    }

    fn finish(&self) -> String {
        format_hash(self.0)
    }

    fn value(&self) -> u64 {
        self.0
    }
}

fn hex(bytes: &[u8]) -> String {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    let mut output = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        output.push(DIGITS[(byte >> 4) as usize] as char);
        output.push(DIGITS[(byte & 15) as usize] as char);
    }
    output
}

fn unhex(value: &str) -> Result<Vec<u8>> {
    if !value.len().is_multiple_of(2) {
        return Err(PixyError::Transport("invalid hex data".into()));
    }
    value
        .as_bytes()
        .chunks_exact(2)
        .map(|pair| {
            let high = digit(pair[0])?;
            let low = digit(pair[1])?;
            Ok((high << 4) | low)
        })
        .collect()
}

fn compress(bytes: &[u8]) -> Result<Vec<u8>> {
    let mut encoder = flate2::write::DeflateEncoder::new(Vec::new(), flate2::Compression::best());
    encoder.write_all(bytes).map_err(asset_error)?;
    encoder.finish().map_err(asset_error)
}

fn decompress(bytes: &[u8], raw_size: usize) -> Result<Vec<u8>> {
    if raw_size > MAX_ITEM_SIZE {
        return Err(PixyError::Transport("asset exceeds raw size cap".into()));
    }
    let decoder = flate2::read::DeflateDecoder::new(bytes);
    let mut output = Vec::with_capacity(raw_size);
    decoder
        .take((raw_size + 1) as u64)
        .read_to_end(&mut output)
        .map_err(asset_error)?;
    if output.len() > raw_size {
        return Err(PixyError::Transport(
            "asset expands beyond declared size".into(),
        ));
    }
    Ok(output)
}

fn digit(byte: u8) -> Result<u8> {
    match byte {
        b'0'..=b'9' => Ok(byte - b'0'),
        b'a'..=b'f' => Ok(byte - b'a' + 10),
        _ => Err(PixyError::Transport("invalid hex data".into())),
    }
}

fn asset_error(error: std::io::Error) -> PixyError {
    PixyError::Transport(error.to_string())
}
