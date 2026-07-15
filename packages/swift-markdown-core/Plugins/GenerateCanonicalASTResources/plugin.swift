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
                arguments: [specDirectory.path, output.path],
                inputFiles: inputs,
                outputFiles: [output]
            )
        ]
    }
}

private struct PluginFailure: Error, CustomStringConvertible {
    let description: String

    init(_ description: String) {
        self.description = description
    }
}
