// Windows: can wgpu/dawn.node load into this wasmcart-run.exe?
//
// native-dawn's dawn.node delay-loads Node-API from "node.exe" and its
// delay-load hook resolves that to the program loading it. So the addon must
// not import node.exe directly (native-dawn before 0.1.2 did, and could not
// load here), and this executable must export every function the addon takes
// from node.exe. libnode's N-API objects mark those functions
// __declspec(dllexport), so linking libnode.lib exports them; this checks the
// result. A missing export would otherwise surface as a crash on the cart's
// first WebGPU call.
//
// Run: node test/wgpu_exports.mjs build/wasmcart-run.exe
import fs from 'node:fs'
import path from 'node:path'

const exe = process.argv[2]
if (!exe) throw new Error('usage: node test/wgpu_exports.mjs path/to/wasmcart-run.exe')
const addon = path.join(path.dirname(exe), 'wgpu', 'dawn.node')

// Import, delay-load import and export names of a PE32+ file.
function readPe(file) {
  const b = fs.readFileSync(file)
  const pe = b.readUInt32LE(0x3c)
  if (b.readUInt16LE(0) !== 0x5a4d || b.readUInt32LE(pe) !== 0x4550) throw new Error(`${file}: not a PE file`)
  const optional = pe + 24
  if (b.readUInt16LE(optional) !== 0x20b) throw new Error(`${file}: not PE32+`)
  const dir = i => b.readUInt32LE(optional + 112 + i * 8)
  const sectionTable = optional + b.readUInt16LE(pe + 20)
  const sections = []
  for (let i = 0; i < b.readUInt16LE(pe + 6); i++) {
    const s = sectionTable + i * 40
    sections.push({ va: b.readUInt32LE(s + 12), size: Math.max(b.readUInt32LE(s + 8), b.readUInt32LE(s + 16)), raw: b.readUInt32LE(s + 20) })
  }
  const off = rva => {
    const s = sections.find(s => rva >= s.va && rva < s.va + s.size)
    if (!s) throw new Error(`${file}: RVA 0x${rva.toString(16)} is in no section`)
    return rva - s.va + s.raw
  }
  const str = rva => { const o = off(rva); return b.toString('latin1', o, b.indexOf(0, o)) }
  const names = rva => {
    const out = []
    for (let o = off(rva); b.readBigUInt64LE(o) !== 0n; o += 8) {
      const t = b.readBigUInt64LE(o)
      out.push(t >> 63n ? `#${Number(t & 0xffffn)}` : str(Number(t & 0x7fffffffn) + 2))
    }
    return out
  }
  const imports = new Map(), delayImports = new Map(), exports = new Set()
  if (dir(1)) for (let o = off(dir(1)); b.readUInt32LE(o + 12); o += 20) imports.set(str(b.readUInt32LE(o + 12)).toLowerCase(), names(b.readUInt32LE(o) || b.readUInt32LE(o + 16)))
  if (dir(13)) for (let o = off(dir(13)); b.readUInt32LE(o + 4); o += 32) delayImports.set(str(b.readUInt32LE(o + 4)).toLowerCase(), names(b.readUInt32LE(o + 16)))
  if (dir(0)) {
    const o = off(dir(0)), table = off(b.readUInt32LE(o + 32))
    for (let i = 0; i < b.readUInt32LE(o + 24); i++) exports.add(str(b.readUInt32LE(table + i * 4)))
  }
  return { imports, delayImports, exports }
}

const dawn = readPe(addon)
if (dawn.imports.has('node.exe')) throw new Error(`${addon} imports node.exe directly, so it only loads into node.exe; it needs native-dawn 0.1.2 or newer`)
const wanted = dawn.delayImports.get('node.exe')
if (!wanted?.length) throw new Error(`${addon} does not delay-load Node-API from node.exe`)
const { exports } = readPe(exe)
const missing = wanted.filter(n => !exports.has(n))
if (missing.length) throw new Error(`${exe} does not export what dawn.node needs from it: ${missing.join(', ')}`)
console.log(`PASS: ${path.basename(exe)} exports all ${wanted.length} Node-API functions dawn.node delay-loads`)
