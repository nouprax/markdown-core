// Worker-side half of the concurrency test: each worker thread imports the
// module fresh (its own WASM instance and scratch) and replays its jobs
// through line-by-line edits, returning canonical dumps for comparison.
import { parentPort, workerData } from "node:worker_threads";
import { Document } from "../dist/index.js";

const dumps = workerData.jobs.map(({ source, options }) => {
    let document = Document("", options);
    let streamed = "";
    for (const chunk of source.split(/(?<=\n)/)) {
        streamed += chunk;
        const previous = document;
        document = document.edit(streamed);
        previous.close();
    }
    const dump = document.dump();
    document.close();
    return dump;
});
parentPort.postMessage(dumps);
