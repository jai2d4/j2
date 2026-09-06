import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const frontendDir = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const source = path.join(frontendDir, "index.html");
const outputDir = path.join(frontendDir, "dist");

fs.rmSync(outputDir, { recursive: true, force: true });
fs.mkdirSync(outputDir, { recursive: true });
fs.copyFileSync(source, path.join(outputDir, "index.html"));

console.log("Built frontend/dist/index.html");
