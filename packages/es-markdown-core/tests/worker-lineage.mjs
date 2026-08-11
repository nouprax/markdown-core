// Worker-side half of the lineage uniqueness test: each worker thread
// imports the module fresh — its own WASM instance whose allocator state and
// coarse clocks repeat exactly across workers — and reports the lineage of
// its first parse. Only host entropy can keep these distinct.
import { parentPort } from "node:worker_threads";
import { Document } from "../dist/index.js";

const document = Document("");
try {
    parentPort.postMessage(document.id.lineage.toString());
} finally {
    document.close();
}
