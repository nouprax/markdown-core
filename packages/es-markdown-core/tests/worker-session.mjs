// Worker-side half of the session concurrency test: each worker thread
// imports the module fresh (its own WASM instance and per-session scratch)
// and replays its jobs through line-streamed sessions, returning canonical
// dumps for comparison.
import { parentPort, workerData } from "node:worker_threads";
import { MarkupSession } from "../dist/index.js";

const dumps = workerData.jobs.map(({ source, options }) => {
    const session = new MarkupSession(options);
    try {
        for (const chunk of source.split(/(?<=\n)/)) {
            session.append(chunk);
            session.commit();
        }
        return session.document.dump();
    } finally {
        session.close();
    }
});
parentPort.postMessage(dumps);
