import Foundation
import MarkdownCore
import MarkdownCoreLSP
import Testing

/// The protocol transcripts (specs/lsp): each client message a binding can be
/// handed, replayed through ``Server`` in process, must get the answer the
/// JSON-RPC adapter gives. A body that is not JSON, or not a message, is the
/// adapter's alone to read, so it and its answer are left out.
@Suite("transcripts") struct TranscriptSuite {
    @Test("the server answers every transcript as the JSON-RPC adapter does")
    func transcripts() throws {
        let resource = try #require(Bundle.module.url(forResource: "lsp-transcripts", withExtension: "json"))
        let bundled = try JSONDecoder().decode([Bundled].self, from: Data(contentsOf: resource))
        #expect(!bundled.isEmpty)
        for transcript in bundled {
            try replay(transcript)
        }
    }

    @Test("edits are checked whole, and refused ones leave the document as it was")
    func edits() throws {
        let server = try Server()
        _ = try server.initialize([.utf8])
        try server.open("file:///a.md", version: 1, text: "one\n")
        let start = MarkdownCoreLSP.Position(line: 0, character: 3)
        // MarkdownCore has a Position and Scope of its own.
        let origin = MarkdownCoreLSP.Position(line: 0, character: 0)
        let backwards = Edit(scope: MarkdownCoreLSP.Scope(start: start, end: origin), text: "")
        let append = Edit(scope: MarkdownCoreLSP.Scope(start: start, end: start), text: " two")
        #expect(throws: ServerError(code: .invalidParams)) {
            try server.update("file:///a.md", version: 2, edits: [append, backwards])
        }
        #expect(try server.complete("file:///a.md", at: start).version == 1)
        try server.update("file:///a.md", version: 3, edits: [append, Edit(scope: nil, text: "whole")])
        #expect(try server.complete("file:///a.md", at: start).version == 3)
        try server.close("file:///a.md")
        #expect(throws: ServerError(code: .invalidParams)) {
            try server.complete("file:///a.md", at: start)
        }
    }
}

private struct Bundled: Decodable {
    let name: String
    let text: String
}

private func replay(_ transcript: Bundled) throws {
    let root = try #require(
        try JSONSerialization.jsonObject(with: Data(transcript.text.utf8)) as? [String: Any]
    )
    let messages = try #require(root["messages"] as? [[String: Any]])
    let host = try Host()
    var index = 0
    while index < messages.count {
        let entry = messages[index]
        index += 1
        if let code = entry["exit"] as? Int {
            #expect(host.exitCode == code, "\(transcript.name): the exit code")
            continue
        }
        guard let message = entry["client"] as? [String: Any], Host.isMessage(message) else {
            // The adapter's alone: skip its answer too.
            if index < messages.count, messages[index]["server"] != nil {
                index += 1
            }
            continue
        }
        let answer = host.handle(message)
        let expected = index < messages.count ? messages[index]["server"] : nil
        if expected != nil {
            index += 1
        }
        switch (answer, expected) {
        case (nil, nil):
            break
        case (let answer?, let expected?):
            #expect(
                NSDictionary(dictionary: answer).isEqual(expected),
                "\(transcript.name), message \(index): \(answer) is not \(expected)"
            )
        default:
            Issue.record("\(transcript.name), message \(index): answered \(String(describing: answer))")
        }
    }
}

/// The adapter's mapping of messages onto the engine (lsp/adapter.c),
/// written over ``Server``.
private final class Host {
    let server: Server

    init() throws {
        server = try Server()
    }

    var exitCode: Int { server.phase == .halted ? 0 : 1 }

    /// Whether `message` is a request or a notification, which a binding can
    /// be handed.
    static func isMessage(_ message: [String: Any]) -> Bool {
        guard message["jsonrpc"] as? String == "2.0", message["method"] is String else { return false }
        guard let id = message["id"] else { return true }
        return id is String || (id is NSNumber && !(id is NSNull))
    }

    func handle(_ message: [String: Any]) -> [String: Any]? {
        // isMessage checked that the method is a string.
        let method = message["method"] as? String ?? ""
        let params = message["params"]
        guard let id = message["id"] else {
            notify(method, params as? [String: Any])
            return nil
        }
        do {
            return ["jsonrpc": "2.0", "id": id, "result": try request(method, params)]
        } catch let error as ServerError {
            return failure(id, Int(error.code.rawValue))
        } catch let failure as Failure {
            return self.failure(id, failure.code)
        } catch {
            return failure(id, -32603)
        }
    }

    private func request(_ method: String, _ params: Any?) throws -> Any {
        switch method {
        case "initialize":
            guard let params = params as? [String: Any] else { throw Failure.invalidParams }
            let capabilities = try server.initialize(Host.units(params))
            return [
                "capabilities": [
                    "positionEncoding": capabilities.unit == .utf8 ? "utf-8" : "utf-16",
                    "textDocumentSync": ["openClose": true, "change": 2],
                    "completionProvider": ["triggerCharacters": capabilities.cues.map { String($0) }],
                ],
                "serverInfo": ["name": "markdown-core"],
            ]
        case "shutdown":
            try server.shutdown()
            return NSNull()
        case "textDocument/completion":
            let params = params as? [String: Any]
            guard let uri = Host.uri(params), let position = Host.position(params?["position"]) else {
                throw Failure.invalidParams
            }
            _ = try server.complete(uri, at: position)
            return ["isIncomplete": false, "items": [Any]()]
        default:
            switch server.phase {
            case .fresh: throw Failure(code: -32002)
            case .halted: throw Failure(code: -32600)
            case .active: throw Failure(code: -32601)
            }
        }
    }

    /// The position encodings the client offers, at most two, as the adapter
    /// reads them.
    private static func units(_ params: [String: Any]) -> [TextUnit] {
        let general = (params["capabilities"] as? [String: Any])?["general"] as? [String: Any]
        var units: [TextUnit] = []
        for name in general?["positionEncodings"] as? [Any] ?? [] where units.count < 2 {
            if name as? String == "utf-8" {
                units.append(.utf8)
            } else if name as? String == "utf-16" {
                units.append(.utf16)
            }
        }
        return units
    }

    /// A notification answers nothing; one that fails is dropped.
    private func notify(_ method: String, _ params: [String: Any]?) {
        let document = params?["textDocument"] as? [String: Any]
        switch method {
        case "textDocument/didOpen":
            guard let uri = Host.uri(params), let text = document?["text"] as? String,
                let version = Host.integer(document?["version"], Int64(Int32.min), Int64(Int32.max))
            else { return }
            try? server.open(uri, version: Int32(version), text: text)
        case "textDocument/didChange":
            guard let uri = Host.uri(params), let edits = Host.edits(params?["contentChanges"]),
                let version = Host.integer(document?["version"], Int64(Int32.min), Int64(Int32.max))
            else { return }
            try? server.update(uri, version: Int32(version), edits: edits)
        case "textDocument/didClose":
            guard let uri = Host.uri(params) else { return }
            try? server.close(uri)
        default:
            break
        }
    }

    /// The content changes of didChange, or nil when one is not the
    /// protocol's.
    private static func edits(_ value: Any?) -> [Edit]? {
        guard let list = value as? [Any] else { return nil }
        var edits: [Edit] = []
        for item in list {
            guard let item = item as? [String: Any], let text = item["text"] as? String else { return nil }
            guard let range = item["range"] else {
                edits.append(Edit(scope: nil, text: text))
                continue
            }
            guard let range = range as? [String: Any], let start = position(range["start"]),
                let end = position(range["end"])
            else { return nil }
            edits.append(Edit(scope: MarkdownCoreLSP.Scope(start: start, end: end), text: text))
        }
        return edits
    }

    private func failure(_ id: Any, _ code: Int) -> [String: Any] {
        ["jsonrpc": "2.0", "id": id, "error": ["code": code, "message": Host.reason(code)]]
    }

    private static func reason(_ code: Int) -> String {
        switch code {
        case -32700: "the message is not JSON"
        case -32600: "the message is not a request the server takes now"
        case -32601: "the server has no such method"
        case -32602: "the params are not the method's"
        case -32002: "the server is not initialized"
        default: "the server failed"
        }
    }

    private static func uri(_ params: [String: Any]?) -> String? {
        (params?["textDocument"] as? [String: Any])?["uri"] as? String
    }

    private static func position(_ value: Any?) -> MarkdownCoreLSP.Position? {
        let value = value as? [String: Any]
        guard let line = integer(value?["line"], 0, Int64(UInt32.max)),
            let character = integer(value?["character"], 0, Int64(UInt32.max))
        else { return nil }
        return MarkdownCoreLSP.Position(line: UInt32(line), character: UInt32(character))
    }

    /// A JSON integer within `min...max`, as the adapter reads one.
    private static func integer(_ value: Any?, _ min: Int64, _ max: Int64) -> Int64? {
        // A boolean is a number to Foundation, and not to JSON.
        guard let number = value as? NSNumber, String(cString: number.objCType) != "c" else { return nil }
        let integer = number.int64Value
        guard NSNumber(value: integer) == number, integer >= min, integer <= max else { return nil }
        return integer
    }
}

private struct Failure: Error {
    let code: Int
    static let invalidParams = Failure(code: -32602)
}
