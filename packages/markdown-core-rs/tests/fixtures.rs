//! Every example of the C package's correctness fixtures
//! (`packages/markdown-core/tests/fixtures/*.txt`) must dump exactly as its
//! reviewed expected block.
//!
//! An example listed in `tests/departures.txt`, where this parser follows
//! the letter of the CommonMark spec and C does not, dumps as the entry
//! there says instead.
//!
//! `FIXTURES_SHOW=N` prints the first N differences; `FIXTURES_ONLY=name`
//! runs one fixture file; `FIXTURES_EXAMPLE=N` runs one example of it.

use std::{collections::BTreeMap, env, fs, path::Path};

struct Example {
    number: usize,
    line: usize,
    markdown: String,
    expected: String,
}

const FENCE: &str = "````````````````````````````````";

/// The examples of a fixture file, as the C loader reads them.
fn load(text: &str) -> Vec<Example> {
    let mut examples = vec![];
    let mut state = 0;
    let mut disabled = false;
    let mut number = 0;
    let mut start = 0;
    let mut markdown = String::new();
    let mut expected = String::new();
    for (index, raw) in text.split('\n').enumerate() {
        let line = raw.trim_end_matches(['\r', ' ', '\t']);
        if line.len() >= 40 && line.starts_with(FENCE) && line[32..].starts_with(" example") {
            state = 1;
            start = index + 1;
            disabled = line[40..].split(' ').any(|word| word == "disabled");
        } else if line == FENCE && state != 0 {
            number += 1;
            if !disabled {
                examples.push(Example {
                    number,
                    line: start,
                    markdown: markdown.replace('\u{2192}', "\t").replace('\u{2400}', "\0"),
                    expected: expected.replace('\u{2192}', "\t"),
                });
            }
            markdown.clear();
            expected.clear();
            state = 0;
        } else if state != 0 && line == "." {
            state = 2;
        } else if state == 1 {
            markdown.push_str(raw);
            markdown.push('\n');
        } else if state == 2 {
            expected.push_str(raw);
            expected.push('\n');
        }
    }
    examples
}

/// The expected dumps of `tests/departures.txt`, by fixture file and
/// example number.
fn departures() -> BTreeMap<(String, usize), String> {
    let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/departures.txt");
    let text = fs::read_to_string(path).expect("expected the departures");
    let mut entries = BTreeMap::new();
    let mut key: Option<(String, usize)> = None;
    let mut expected: Option<String> = None;
    for line in text.split('\n') {
        if let Some(rest) = line.strip_prefix(FENCE) {
            if let Some(rest) = rest.strip_prefix(" departure ") {
                let (name, number) = rest.split_once(' ').expect("expected a file and a number");
                key = Some((name.to_string(), number.parse().expect("expected a number")));
            } else if let (Some(key), Some(expected)) = (key.take(), expected.take()) {
                entries.insert(key, expected);
            }
        } else if key.is_some() {
            match &mut expected {
                Some(expected) => {
                    expected.push_str(line);
                    expected.push('\n');
                }
                None if line == "." => expected = Some(String::new()),
                None => {}
            }
        }
    }
    entries
}

#[test]
fn fixtures() {
    let directory = Path::new(env!("CARGO_MANIFEST_DIR")).join("../markdown-core/tests/fixtures");
    let show: usize = env::var("FIXTURES_SHOW")
        .ok()
        .and_then(|value| value.parse().ok())
        .unwrap_or(0);
    let filter = env::var("FIXTURES_ONLY").ok();
    let example = env::var("FIXTURES_EXAMPLE")
        .ok()
        .and_then(|value| value.parse::<usize>().ok());
    let mut names: Vec<String> = fs::read_dir(&directory)
        .expect("expected the C fixtures")
        .filter_map(|entry| {
            let name = entry.ok()?.file_name().into_string().ok()?;
            name.ends_with(".txt").then_some(name)
        })
        .collect();
    names.sort();
    let mut total = 0;
    let mut passed = 0;
    let mut shown = 0;
    let mut report = String::new();
    let departures = departures();
    for name in &names {
        if let Some(filter) = &filter {
            if !name.starts_with(filter.as_str()) {
                continue;
            }
        }
        let text = fs::read_to_string(directory.join(name)).expect("expected a fixture");
        let mut file_total = 0;
        let mut file_passed = 0;
        for case in load(&text) {
            if example.is_some_and(|number| number != case.number) {
                continue;
            }
            file_total += 1;
            let expected = match departures.get(&(name.clone(), case.number)) {
                Some(entry) => {
                    assert_ne!(
                        entry, &case.expected,
                        "{name} {}: the departure is stale",
                        case.number
                    );
                    entry
                }
                None => &case.expected,
            };
            let document = markdown_core::parse(&case.markdown);
            let actual = markdown_core::dump::dump(&document, &case.markdown);
            if &actual == expected {
                file_passed += 1;
            } else if shown < show {
                shown += 1;
                println!(
                    "--- {} example {} (line {})\n{}--- expected\n{}--- actual\n{}",
                    name, case.number, case.line, case.markdown, expected, actual
                );
            }
        }
        report.push_str(&format!("{:>4}/{:<4} {}\n", file_passed, file_total, name));
        total += file_total;
        passed += file_passed;
    }
    println!("{}{}/{} examples dump as expected", report, passed, total);
    assert_eq!(passed, total, "every fixture example must dump as expected");
}
