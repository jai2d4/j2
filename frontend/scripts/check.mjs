import fs from "node:fs";

const html = fs.readFileSync(new URL("../index.html", import.meta.url), "utf8");
const script = html.match(/<script>([\s\S]*?)<\/script>/);

if (!script) {
  throw new Error("frontend/index.html does not contain an inline application script");
}

new Function(script[1]);

if (!html.includes('location.port === "5173"')) {
  throw new Error("frontend/index.html does not route local development API calls to port 8000");
}

for (const required of ["CREATE", "PROFILE", "Film", "AI TRUTH REPORT"]) {
  if (!html.includes(required)) {
    throw new Error(`frontend/index.html is missing required UI text: ${required}`);
  }
}

console.log("TruGrade standalone frontend check passed");
