import Foundation
import PackagePlugin

@main
struct GenerateCanonicalASTResources: BuildToolPlugin {
    func createBuildCommands(context: PluginContext, target: Target) async throws -> [Command] {
        let specDirectory =
            context.package.directoryURL
            .appendingPathComponent("specs/canonical-ast", isDirectory: true)
        let output =
            context.pluginWorkDirectoryURL
            .appendingPathComponent("canonical-ast-fixtures.json", isDirectory: false)
        let queries =
            context.package.directoryURL
            .appendingPathComponent("specs/context/queries.json", isDirectory: false)
        let queriesOutput =
            context.pluginWorkDirectoryURL
            .appendingPathComponent("context-queries.json", isDirectory: false)
        let transcripts =
            context.package.directoryURL
            .appendingPathComponent("specs/lsp/transcripts", isDirectory: true)
        let transcriptsOutput =
            context.pluginWorkDirectoryURL
            .appendingPathComponent("lsp-transcripts.json", isDirectory: false)
        let transcriptInputs = try Self.transcriptFiles(in: transcripts)
        let tool = try context.tool(named: "CanonicalASTResourceGenerator")

        guard
            let enumerator = FileManager.default.enumerator(
                at: specDirectory,
                includingPropertiesForKeys: [.isRegularFileKey],
                options: [.skipsHiddenFiles]
            )
        else {
            throw PluginFailure("cannot enumerate canonical AST spec at \(specDirectory.path)")
        }

        let inputs = try enumerator.compactMap { element -> URL? in
            guard let url = element as? URL else { return nil }
            let values = try url.resourceValues(forKeys: [.isRegularFileKey])
            return values.isRegularFile == true ? url : nil
        }.sorted { $0.path < $1.path }

        guard inputs.contains(where: { $0.lastPathComponent == "manifest.json" }) else {
            throw PluginFailure("canonical AST manifest is missing from \(specDirectory.path)")
        }

        return [
            .buildCommand(
                displayName: "Generate canonical AST conformance resource",
                executable: tool.url,
                arguments: [
                    specDirectory.path, output.path, queries.path, queriesOutput.path, transcripts.path,
                    transcriptsOutput.path,
                ],
                inputFiles: inputs + [queries] + transcriptInputs,
                outputFiles: [output, queriesOutput, transcriptsOutput]
            )
        ]
    }

    /// The transcripts' JSON files, in path order.
    private static func transcriptFiles(in directory: URL) throws -> [URL] {
        try FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil)
            .filter { $0.pathExtension == "json" }
            .sorted { $0.path < $1.path }
    }
}

private struct PluginFailure: Error, CustomStringConvertible {
    let description: String

    init(_ description: String) {
        self.description = description
    }
}
