// The Swift timing lane: source string to value tree through the public
// entry, on the workloads the C timing lane reads, so the two measure the
// same bytes. Opt-in, informational, never a gate:
// `pnpm benchmark:swift` (`swift run -c release MarkdownCoreBenchmarks`).
//
//   MarkdownCoreBenchmarks [--samples DIR] [--copies N] [--repeats N]
//       [--warmup N] [--case NAME] [--json FILE] [FILE...]
//
// Each case reports the parse (source in, tree out) and a walk of the tree
// with an empty visitor, the minimum and the median of every repeat, the
// throughput from the bytes and the time per node, and the SHA-256 of the
// input so a measurement can prove which bytes it read.
import Foundation
import MarkdownCore

/// Every kind's callback counts and nothing else: the walk measures the traversal and the dispatch.
struct CountingVisitor: MarkupVisitor {
    var visits = 0

    mutating func visit(_ node: Document, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: Callout, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: Paragraph, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: Heading, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: ThematicBreak, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: MarkdownCore.List, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: ListItem, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: CodeBlock, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: HTMLBlock, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: FormulaBlock, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: Table, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: DirectiveBlock, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: DirectiveLabel, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: Text, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: SoftBreak, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: LineBreak, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: Code, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: HTML, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: Comment, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: CrossLink, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: CrossEmbedded, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: Formula, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: Emphasis, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: Strong, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: Strikethrough, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: Mark, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: Insertion, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: Span, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: Superscript, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: Subscript, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: DefinitionList, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: Definition, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: Link, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: Embedded, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: Directive, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: Cite, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: TableCaption, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: TableRow, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: TableCell, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: Citation, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: Footnote, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: Specimen, phase: MarkupVisitPhase) { visits += 1 }
    mutating func visit(_ node: Metadata, phase: MarkupVisitPhase) { visits += 1 }
}

struct Case {
    let name: String
    let generator: String
    let parameters: String
    let source: String
}

struct Sample {
    let parseNs: UInt64
    let walkNs: UInt64
}

let baselineUnit = "## Section\n\nParagraph with **strong**, [link](https://example.com), and 🚀.\n\n"

func nanoseconds() -> UInt64 {
    var time = timespec()
    clock_gettime(CLOCK_MONOTONIC, &time)
    return UInt64(time.tv_sec) * 1_000_000_000 + UInt64(time.tv_nsec)
}

// SHA-256 as the C lane and the other lanes compute it, over the UTF-8 bytes.
func sha256(_ text: String) -> String {
    var state = SHA256.initialState
    var message = Array(text.utf8)
    let bitLength = UInt64(message.count) * 8
    message.append(0x80)
    while message.count % 64 != 56 { message.append(0) }
    for shift in stride(from: 56, through: 0, by: -8) { message.append(UInt8((bitLength >> UInt64(shift)) & 0xff)) }
    for chunk in stride(from: 0, to: message.count, by: 64) {
        SHA256.compress(message[chunk..<(chunk + 64)], into: &state)
    }
    return state.map { String(format: "%08x", $0) }.joined()
}

enum SHA256 {
    static let initialState: [UInt32] = [
        0x6a09_e667, 0xbb67_ae85, 0x3c6e_f372, 0xa54f_f53a, 0x510e_527f, 0x9b05_688c, 0x1f83_d9ab, 0x5be0_cd19,
    ]

    static let roundConstants: [UInt32] = [
        0x428a_2f98, 0x7137_4491, 0xb5c0_fbcf, 0xe9b5_dba5, 0x3956_c25b, 0x59f1_11f1, 0x923f_82a4, 0xab1c_5ed5,
        0xd807_aa98, 0x1283_5b01, 0x2431_85be, 0x550c_7dc3, 0x72be_5d74, 0x80de_b1fe, 0x9bdc_06a7, 0xc19b_f174,
        0xe49b_69c1, 0xefbe_4786, 0x0fc1_9dc6, 0x240c_a1cc, 0x2de9_2c6f, 0x4a74_84aa, 0x5cb0_a9dc, 0x76f9_88da,
        0x983e_5152, 0xa831_c66d, 0xb003_27c8, 0xbf59_7fc7, 0xc6e0_0bf3, 0xd5a7_9147, 0x06ca_6351, 0x1429_2967,
        0x27b7_0a85, 0x2e1b_2138, 0x4d2c_6dfc, 0x5338_0d13, 0x650a_7354, 0x766a_0abb, 0x81c2_c92e, 0x9272_2c85,
        0xa2bf_e8a1, 0xa81a_664b, 0xc24b_8b70, 0xc76c_51a3, 0xd192_e819, 0xd699_0624, 0xf40e_3585, 0x106a_a070,
        0x19a4_c116, 0x1e37_6c08, 0x2748_774c, 0x34b0_bcb5, 0x391c_0cb3, 0x4ed8_aa4a, 0x5b9c_ca4f, 0x682e_6ff3,
        0x748f_82ee, 0x78a5_636f, 0x84c8_7814, 0x8cc7_0208, 0x90be_fffa, 0xa450_6ceb, 0xbef9_a3f7, 0xc671_78f2,
    ]

    static func rotateRight(_ value: UInt32, _ count: UInt32) -> UInt32 {
        (value >> count) | (value << (32 - count))
    }

    static func compress(_ block: ArraySlice<UInt8>, into state: inout [UInt32]) {
        var schedule = [UInt32](repeating: 0, count: 64)
        for word in 0..<16 {
            let base = block.startIndex + word * 4
            schedule[word] =
                UInt32(block[base]) << 24 | UInt32(block[base + 1]) << 16 | UInt32(block[base + 2]) << 8
                | UInt32(block[base + 3])
        }
        for word in 16..<64 {
            let older = schedule[word - 15]
            let newer = schedule[word - 2]
            let sigma0 = rotateRight(older, 7) ^ rotateRight(older, 18) ^ (older >> 3)
            let sigma1 = rotateRight(newer, 17) ^ rotateRight(newer, 19) ^ (newer >> 10)
            schedule[word] = schedule[word - 16] &+ sigma0 &+ schedule[word - 7] &+ sigma1
        }
        var work = state
        for round in 0..<64 {
            let sigma1 = rotateRight(work[4], 6) ^ rotateRight(work[4], 11) ^ rotateRight(work[4], 25)
            let choice = (work[4] & work[5]) ^ (~work[4] & work[6])
            let temp1 = work[7] &+ sigma1 &+ choice &+ roundConstants[round] &+ schedule[round]
            let sigma0 = rotateRight(work[0], 2) ^ rotateRight(work[0], 13) ^ rotateRight(work[0], 22)
            let majority = (work[0] & work[1]) ^ (work[0] & work[2]) ^ (work[1] & work[2])
            let temp2 = sigma0 &+ majority
            work[7] = work[6]
            work[6] = work[5]
            work[5] = work[4]
            work[4] = work[3] &+ temp1
            work[3] = work[2]
            work[2] = work[1]
            work[1] = work[0]
            work[0] = temp1 &+ temp2
        }
        for index in 0..<8 { state[index] &+= work[index] }
    }
}

func escape(_ value: String) -> String {
    var escaped = ""
    for character in value {
        switch character {
        case "\"": escaped += "\\\""
        case "\\": escaped += "\\\\"
        case "\n": escaped += "\\n"
        default: escaped.append(character)
        }
    }
    return escaped
}

struct Options {
    var samples = "packages/markdown-core/benchmarks/samples"
    var copies = 200
    var repeats = 20
    var warmup = 3
    var only: String?
    var json: String?
    var files: [String] = []
}

func parseOptions(_ arguments: [String]) -> Options {
    var options = Options()
    var index = 0
    func value() -> String {
        index += 1
        precondition(index < arguments.count, "\(arguments[index - 1]) needs a value")
        return arguments[index]
    }
    while index < arguments.count {
        let argument = arguments[index]
        switch argument {
        case "--samples": options.samples = value()
        case "--copies": options.copies = Int(value()) ?? options.copies
        case "--repeats": options.repeats = Int(value()) ?? options.repeats
        case "--warmup": options.warmup = Int(value()) ?? options.warmup
        case "--case": options.only = value()
        case "--json": options.json = value()
        default: options.files.append(argument)
        }
        index += 1
    }
    return options
}

let options = parseOptions(Array(CommandLine.arguments.dropFirst()))
let samplesDirectory = options.samples
let copies = options.copies
let repeats = options.repeats
let warmup = options.warmup
let only = options.only
let jsonPath = options.json
let files = options.files

// The same generators as `tests/support/bench_workloads.c`, byte for byte:
// the baseline unit repeated, and a tracked sample repeated with a blank
// line between copies so the repeated shape is the sample's shape.
var cases: [Case] = [
    Case(
        name: "binding_baseline",
        generator: "binding_baseline",
        parameters: "scale=2000",
        source: String(repeating: baselineUnit, count: 2000)
    ),
    Case(name: "empty_document", generator: "empty_document", parameters: "", source: ""),
]
let sampleNames = ((try? FileManager.default.contentsOfDirectory(atPath: samplesDirectory)) ?? [])
    .filter { $0.hasSuffix(".md") }.sorted()
for name in sampleNames {
    let path = (samplesDirectory as NSString).appendingPathComponent(name)
    guard let sample = try? String(contentsOfFile: path, encoding: .utf8) else { continue }
    cases.append(
        Case(
            name: name,
            generator: "sample",
            parameters: "copies=\(copies)",
            source: String(repeating: sample + "\n\n", count: copies)
        )
    )
}
for file in files {
    guard let source = try? String(contentsOfFile: file, encoding: .utf8) else {
        FileHandle.standardError.write(Data("cannot read \(file)\n".utf8))
        exit(2)
    }
    cases.append(Case(name: (file as NSString).lastPathComponent, generator: "file", parameters: file, source: source))
}

var results: [String] = []
for item in cases {
    if let only, item.name != only { continue }
    let bytes = item.source.utf8.count
    var visitor = CountingVisitor()
    for _ in 0..<warmup {
        let document = try Document.parse(item.source)
        document.walk(with: &visitor)
    }
    var samples: [Sample] = []
    var nodes = 0
    for _ in 0..<repeats {
        let parseStart = nanoseconds()
        let document = try Document.parse(item.source)
        let parseEnd = nanoseconds()
        visitor.visits = 0
        document.walk(with: &visitor)
        let walkEnd = nanoseconds()
        nodes = visitor.visits / 2
        samples.append(Sample(parseNs: parseEnd - parseStart, walkNs: walkEnd - parseEnd))
    }
    let parses = samples.map { $0.parseNs }.sorted()
    let walks = samples.map { $0.walkNs }.sorted()
    let minParse = parses[0]
    let medianParse = parses[parses.count / 2]
    let minWalk = walks[0]
    let medianWalk = walks[walks.count / 2]
    let mbPerSecond = minParse > 0 ? String(format: "%.3f", Double(bytes) / 1e6 / (Double(minParse) / 1e9)) : "0"
    let nsPerNode = nodes > 0 ? String(format: "%.3f", Double(minParse) / Double(nodes)) : "0"
    let digest = sha256(item.source)
    print(
        "benchmark case=\(item.name) bytes=\(bytes) nodes=\(nodes) repeats=\(repeats) warmup=\(warmup) "
            + "min_parse_ns=\(minParse) median_parse_ns=\(medianParse) min_walk_ns=\(minWalk) "
            + "median_walk_ns=\(medianWalk) mb_per_s=\(mbPerSecond) ns_per_node=\(nsPerNode) sha256=\(digest)"
    )
    let sampleText = samples.map { "{\"parseNs\": \($0.parseNs), \"walkNs\": \($0.walkNs)}" }.joined(separator: ", ")
    results.append(
        """
            {
              "name": "\(escape(item.name))",
              "generator": "\(item.generator)",
              "parameters": "\(escape(item.parameters))",
              "bytes": \(bytes),
              "inputSha256": "\(digest)",
              "nodes": \(nodes),
              "minParseNs": \(minParse),
              "medianParseNs": \(medianParse),
              "minWalkNs": \(minWalk),
              "medianWalkNs": \(medianWalk),
              "mbPerSecond": \(mbPerSecond),
              "nsPerNode": \(nsPerNode),
              "samples": [\(sampleText)]
            }
        """
    )
}
if let jsonPath {
    let document = """
        {
          "schema": 2,
          "runtime": "swift",
          "lane": "timing",
          "workload": "binding",
          "workloadVersion": 1,
          "warmup": \(warmup),
          "repeats": \(repeats),
          "cases": [
        \(results.joined(separator: ",\n"))
          ]
        }

        """
    try document.write(toFile: jsonPath, atomically: true, encoding: .utf8)
}
