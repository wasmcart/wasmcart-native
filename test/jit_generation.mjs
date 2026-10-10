// The restore generation (wc_jit_config key 7) the native host computes must be
// the one wasmcart's Node and browser hosts compute (SPEC.md known answer):
// evaluate src/jit_js.h's library, the exact source the player's V8 runs.
import { readFileSync } from 'node:fs';
const h = readFileSync(new URL('../src/jit_js.h', import.meta.url), 'utf8');
const js = h.slice(h.indexOf('R"WCJS(') + 7, h.lastIndexOf(')WCJS"'));
const { restoreGeneration } = (0, eval)(js);
const kat = new Uint8Array(65536);
for (let i = 0; i < kat.length; i++) kat[i] = (i * 31 + 7) & 255;
const g = restoreGeneration(kat.buffer, 0);
if (g === 1954405829) console.log('  ok    restore generation known answer (same as Node and browser)');
else { console.log(`*** FAIL restore generation known answer: ${g}, wanted 1954405829`); process.exit(1); }
