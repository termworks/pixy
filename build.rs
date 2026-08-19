use flate2::{Compression, GzBuilder};
use std::fs;
use std::io::{self, Write};
use std::path::{Path, PathBuf};

const MAGIC: &[u8; 4] = b"HXSP";
const VERSION: u32 = 1;
const MAX_SPRITES: usize = 4096;
const MAX_SPRITE_SIZE: usize = 1024 * 1024;

struct Entry {
    kind: u8,
    name: String,
    raw_len: u32,
    compressed: Vec<u8>,
}

fn main() {
    println!("cargo:rerun-if-changed=assets/pokemon");
    let output = PathBuf::from(std::env::var_os("OUT_DIR").expect("OUT_DIR")).join("pokemon.pack");
    pack(Path::new("assets/pokemon"), &output).expect("pack embedded Pokemon sprites");
}

fn pack(root: &Path, output: &Path) -> io::Result<()> {
    let mut entries = Vec::new();
    for (directory, kind) in [("regular", 0), ("shiny", 1)] {
        let path = root.join(directory);
        let mut names = fs::read_dir(&path)?
            .map(|entry| {
                let entry = entry?;
                if !entry.file_type()?.is_file() {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidData,
                        "Pokemon asset entries must be files",
                    ));
                }
                entry
                    .file_name()
                    .into_string()
                    .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "non-UTF-8 asset name"))
            })
            .collect::<io::Result<Vec<_>>>()?;
        names.sort();
        for name in names {
            if name.is_empty() || name.len() > u8::MAX as usize {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "invalid Pokemon asset name",
                ));
            }
            let raw = fs::read(path.join(&name))?;
            if raw.len() > MAX_SPRITE_SIZE {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "Pokemon asset exceeds size limit",
                ));
            }
            let mut encoder = GzBuilder::new()
                .mtime(0)
                .operating_system(255)
                .write(Vec::new(), Compression::best());
            encoder.write_all(&raw)?;
            entries.push(Entry {
                kind,
                name,
                raw_len: raw.len() as u32,
                compressed: encoder.finish()?,
            });
        }
    }
    if entries.is_empty() || entries.len() > MAX_SPRITES {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "invalid Pokemon asset count",
        ));
    }

    let index_len = entries.iter().try_fold(0_u32, |total, entry| {
        total.checked_add(10 + entry.name.len() as u32)
    });
    let index_len = index_len.ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidData,
            "Pokemon index exceeds size limit",
        )
    })?;
    let mut archive = Vec::new();
    archive.extend_from_slice(MAGIC);
    append_u32(&mut archive, VERSION);
    append_u32(&mut archive, entries.len() as u32);
    append_u32(&mut archive, index_len);
    for entry in &entries {
        archive.push(entry.kind);
        archive.push(entry.name.len() as u8);
        append_u32(&mut archive, entry.raw_len);
        append_u32(&mut archive, entry.compressed.len() as u32);
        archive.extend_from_slice(entry.name.as_bytes());
    }
    for entry in entries {
        archive.extend_from_slice(&entry.compressed);
    }
    fs::write(output, archive)
}

fn append_u32(output: &mut Vec<u8>, value: u32) {
    output.extend_from_slice(&value.to_le_bytes());
}
