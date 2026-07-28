// Worker-side half of the lineage uniqueness test: each worker thread
// imports the module fresh — its own WASM instance whose allocator state and
// coarse clocks repeat exactly across workers — and reports the lineage of
// its first session. Only host entropy can keep these distinct.
import { parentPort } from "node:worker_threads";
import { MarkupSession } from "../dist/index.js";

const session = new MarkupSession();
try {
    parentPort.postMessage(session.lineage.toString());
} finally {
    session.close();
}
