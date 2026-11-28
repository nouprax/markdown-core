use honggfuzz::fuzz;

fn main() {
    loop {
        fuzz!(|data: &[u8]| {
            if let Ok(s) = std::str::from_utf8(data) {
                let _ = markdown_core::dump::dump(&markdown_core::parse(s), s);
            }
        });
    }
}
