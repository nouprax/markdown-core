import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import path from "node:path";

const packageDirectory = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const chromeCandidates = [
    process.env.CHROME,
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    "/usr/bin/google-chrome",
    "/usr/bin/chromium",
    "/usr/bin/chromium-browser"
].filter(Boolean);
let chrome;
for (const candidate of chromeCandidates) {
    try {
        await (await import("node:fs/promises")).access(candidate);
        chrome = candidate;
        break;
    } catch {
        // Try the next platform-specific executable path.
    }
}
if (!chrome) throw new Error("browser suite requires Chrome/Chromium; set CHROME to its executable");

const html = `<!doctype html><meta charset="utf-8"><title>RUNNING</title><body id="result">RUNNING<script type="module">
  try {
    const api = await import('/index.js');
    const parsed = api.Document.parse('# Browser 🌍');
    const valid = parsed.content[0].kind === 'heading' &&
      parsed.content[0].content[0].literal === 'Browser 🌍' &&
      !('memory' in api) && !('initialize' in api);
    document.title = valid ? 'PASS' : 'FAIL';
    document.body.textContent = document.title;
  } catch (error) {
    document.title = 'FAIL'; document.body.textContent = String(error.stack || error);
  } finally {
    await fetch('/result?status=' + encodeURIComponent(document.title));
  }
</script>`;

let reportBrowserResult;
const requests = [];
const server = createServer(async (request, response) => {
    try {
        requests.push(request.url);
        if (request.url === "/") {
            response.setHeader("content-type", "text/html; charset=utf-8");
            response.end(html);
            return;
        }
        if (request.url.startsWith("/result?")) {
            const status = new URL(request.url, "http://127.0.0.1").searchParams.get("status");
            response.end("ok");
            reportBrowserResult?.(status);
            return;
        }
        const name = request.url.slice(1);
        const resolved = path.resolve(packageDirectory, "dist", name);
        if (!resolved.startsWith(path.resolve(packageDirectory, "dist") + path.sep)) {
            response.statusCode = 404;
            response.end("not found");
            return;
        }
        response.setHeader("content-type", name.endsWith(".wasm") ? "application/wasm" : "text/javascript");
        response.end(await readFile(resolved));
    } catch (error) {
        response.statusCode = 500;
        response.end(String(error));
    }
});
await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
let child;
try {
    const { port } = server.address();
    const result = await new Promise((resolve, reject) => {
        let settled = false;
        let stderr = "";
        const finish = (callback, value) => {
            if (settled) return;
            settled = true;
            clearTimeout(timeout);
            callback(value);
        };
        reportBrowserResult = (status) => finish(resolve, status);
        child = spawn(chrome, [
            "--headless=new",
            "--disable-gpu",
            "--no-sandbox",
            "--disable-dev-shm-usage",
            "--disable-background-networking",
            "--disable-component-update",
            "--disable-default-apps",
            "--no-first-run",
            `http://127.0.0.1:${port}/`
        ]);
        child.stderr.setEncoding("utf8").on("data", (chunk) => {
            stderr += chunk;
        });
        const timeout = setTimeout(() => {
            finish(
                reject,
                new Error(
                    `Chrome browser test timed out\nexecutable: ${chrome}\nrequests: ${requests.join(", ")}\n${stderr}`
                )
            );
        }, 60_000);
        child.on("error", (error) => finish(reject, error));
        child.on("close", (status) => {
            finish(reject, new Error(stderr || `Chrome exited with ${status}`));
        });
    });
    assert.equal(result, "PASS", `browser reported ${result}`);
    console.log("browser: Chrome/Chromium ESM + WASM parse passed");
} finally {
    reportBrowserResult = undefined;
    child?.kill();
    await new Promise((resolve) => server.close(resolve));
}
