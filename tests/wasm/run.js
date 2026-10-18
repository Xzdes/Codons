const fs = require("fs");
const path = require("path");

async function main() {
    const wasmPath = process.argv[2];
    const args = process.argv.slice(3).map(Number);
    const bytes = fs.readFileSync(wasmPath);
    const { instance } = await WebAssembly.instantiate(bytes, {});
    const result = instance.exports.run(...args);
    console.log(result);
}

main().catch(err => {
    console.error(err);
    process.exit(1);
});
