// Worker-side half of the multi-instance concurrency test: each worker
// thread imports the module fresh (its own WASM instance and scratch) and
// parses the jobs it was handed, returning canonical dumps for comparison.
import { parentPort, workerData } from "node:worker_threads";
import { Document } from "../dist/index.js";

const dumps = workerData.jobs.map(({ source, options }) => Document.parse(source, options).dump());
parentPort.postMessage(dumps);
