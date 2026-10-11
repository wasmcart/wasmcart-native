// wgpu_bridge.cjs - WebGPU carts in wasmcart-native (SPEC.md, "WebGPU").
//
// The native host already runs every cart inside an embedded Node, so WebGPU
// reuses the reference host's code rather than reimplementing it in C++:
// wasmcart's src/wgpu/host.js runs the emdawnwebgpu glue for the cart, and
// Dawn comes from native-dawn's dawn.node addon. All of it sits in a `wgpu/`
// directory next to the executable (CMake copies it there when built with
// WASMCART_WGPU); this file is the small interface cart_host.cpp calls.
//
// Asynchronous steps return a job ({ done, error, value }); C++ pumps Node's
// event loop until `done`.
'use strict';

const path = require('node:path');

module.exports = function createBridge(dir) {
  const dawn = require(path.join(dir, 'dawn.node'));
  // wasmcart's src/ tree as CMake copies it (host.js imports ../cartMemory.js)
  const host = require(path.join(dir, 'src', 'wgpu', 'host.js'));
  let gpu = null;
  const sessions = new Map();

  const job = fn => {
    const j = { done: false, error: null, value: null };
    Promise.resolve().then(fn).then(v => { j.value = v; }, e => { j.error = String(e?.message || e); })
      .finally(() => { j.done = true; });
    return j;
  };

  // A readback buffer back to the free list, unmapped (or, still mapping,
  // once its map lands).
  const recycle = (ring, f) => {
    if (!f.ready) { f.dropped = true; return; }
    try { f.buffer.unmap(); } catch {}
    if (ring.free.length < 3) ring.free.push(f); else f.buffer.destroy();
  };
  const dropFrame = (ring, f) => recycle(ring, f);

  return {
    isWgpuImportName: host.isWgpuImportName,

    importsWgpu(module) {
      return host.importsWgpu(WebAssembly.Module.imports(module));
    },

    trap: host.gpuImportTrap,

    // Ask for an adapter and build the cart's session. value: the glue's env
    // functions. A failure's message is the reason the host gives the cart.
    prepare(id, module, width, height) {
      return job(async () => {
        if (process.env.WASMCART_NO_WGPU === '1') throw new Error('WebGPU is disabled (WASMCART_NO_WGPU=1)');
        gpu ??= dawn.create([]);
        // WASMCART_WGPU_POWER=low-power|high-performance picks the GPU on a
        // two-GPU machine (wasmcart docs/webgpu.md); the choice is logged so a
        // caller can assert it.
        const adapterOptions = host.wgpuAdapterOptions();
        const adapter = await gpu.requestAdapter(adapterOptions);
        if (!adapter) throw new Error('no WebGPU adapter is available (check the GPU driver)');
        const a = host.describeAdapter(adapter, adapterOptions);
        process.stderr.write(`wasmcart-run: WebGPU on ${a.device || a.description} (${a.vendor}, ${a.featureLevel}${a.powerPreference ? ', ' + a.powerPreference : ''})\n`);
        const rec = { session: null, surface: null, size: [0, 0], lost: null,
          adapter: `${a.device || a.description} (${a.vendor}, ${a.featureLevel})` };
        rec.session = await host.createWgpuSession({
          moduleImports: WebAssembly.Module.imports(module), gpu, adapter, width, height,
          globals: dawn.globals,
          log: msg => process.stderr.write(msg + '\n'),
          onLost: info => { rec.lost = info; },
        });
        sessions.set(id, rec);
        const { session } = rec;
        return session.env;
      });
    },

    // The adapter the cart runs on, for the host's log.
    adapterName(id) {
      return sessions.get(id)?.adapter || '';
    },

    attach(id, instance, memory) {
      return job(() => sessions.get(id).session.attach(instance, memory));
    },

    // "reason: message" once the host's device was lost while the cart ran
    // (wasmcart's session reports it; the host then stops calling wc_render).
    lost(id) {
      const l = sessions.get(id)?.lost;
      return l ? l.reason + (l.message ? ': ' + l.message : '') : null;
    },

    begin(id) {
      sessions.get(id)?.session.beginFrame();
    },

    // An error kept from an async WebGPU callback (a cart-memory allocation
    // the cart could not satisfy, e.g. no wc_alloc/wc_free): its message, once,
    // or null. The host stops the cart with it before the next frame.
    fatal(id) {
      const e = sessions.get(id)?.session.takeFatal?.();
      return e ? String(e.message || e) : null;
    },

    // A window to present into. kind: xlib | wayland | win32 | metal-layer.
    // vsync false (--uncapped) presents without waiting for the display:
    // FIFO under Xwayland measured ~0.65 s per frame.
    attachWindow(id, kind, display, handle, vsync = true) {
      const s = sessions.get(id);
      s.surface?.destroy();
      s.surface = new dawn.NativeSurface(s.session.device, { kind, display, handle, xid: handle });
      s.size = [0, 0];
      s.presentModes = vsync ? ['fifo'] : ['mailbox', 'immediate', 'fifo'];
      return true;
    },

    // Draw the cart's frame into the window, letterboxed into (x, y, w, h)
    // of a winW x winH surface, and present it.
    present(id, x, y, w, h, winW, winH) {
      const s = sessions.get(id);
      if (!s?.surface) return false;
      if (s.size[0] !== winW || s.size[1] !== winH) {
        // The first present mode the surface supports (configure checks).
        let err;
        for (const presentMode of s.presentModes) {
          try {
            s.surface.configure({ width: winW, height: winH, format: gpu.getPreferredCanvasFormat(), usage: 0x10, presentMode });
            err = null;
            break;
          } catch (e) { err = e; }
        }
        if (err) throw err;
        s.size = [winW, winH];
      }
      const texture = s.surface.getCurrentTexture();
      const drew = s.session.drawTo({ getCurrentTexture: () => texture }, { x, y, w, h });
      s.surface.present();
      return drew;
    },

    // The size of the surface the cart renders into: what wc_get_info said at
    // load can differ (an engine that lays out its own project size), and the
    // real frame is this one. null before the cart has configured a surface.
    frameSize(id) {
      const c = sessions.get(id)?.session?.canvas;
      const w = c ? c.width | 0 : 0, h = c ? c.height | 0 : 0;
      return w > 0 && h > 0 ? { width: w, height: h } : null;
    },

    // The cart's last frame as RGBA. value: { width, height, data }.
    read(id) {
      return job(() => sessions.get(id).session.readFrame());
    },

    // Pipelined readback, for a host that shows the frame through another
    // API (the libretro core's GL framebuffer). readQueue starts copying the
    // cart's current frame into one of a few MAP_READ buffers and maps it,
    // without waiting; readTake hands out the OLDEST queued frame once its
    // map has landed (null while it has not), so the host shows frame N-1
    // while the GPU renders N and never waits for a GPU round trip it did
    // not have to. The taken frame stays mapped until readRelease.
    readQueue(id) {
      const s = sessions.get(id);
      const session = s?.session;
      if (!session) return false;
      let texture;
      try { texture = session.context.getCurrentTexture(); } catch { return false; }
      const w = texture.width, h = texture.height, format = texture.format;
      const dev = session.device;
      s.ring ??= { free: [], queued: [], taken: null };
      const ring = s.ring;
      // Up to 3 buffers in flight; a host that does not take them fast
      // enough loses the oldest, never blocks here.
      if (ring.queued.length >= 3) dropFrame(ring, ring.queued.shift());
      const bytesPerRow = Math.ceil(w * 4 / 256) * 256;
      const size = bytesPerRow * h;
      let buffer = null;
      while (ring.free.length) {
        const b = ring.free.pop();
        if (b.size === size && b.dev === dev) { buffer = b.buffer; break; }
        b.buffer.destroy();
      }
      buffer ??= dev.createBuffer({ size, usage: 0x0008 /* COPY_DST */ | 0x0001 /* MAP_READ */ });
      const encoder = dev.createCommandEncoder();
      encoder.copyTextureToBuffer({ texture }, { buffer, bytesPerRow, rowsPerImage: h }, [w, h, 1]);
      dev.queue.submit([encoder.finish()]);
      const f = { buffer, dev, size, width: w, height: h, bytesPerRow, bgra: format.startsWith('bgra'), ready: false, failed: false, dropped: false,
        queuedAt: performance.now(), latency: 0 };
      buffer.mapAsync(0x0001 /* READ */).then(() => {
        f.ready = true;
        f.latency = performance.now() - f.queuedAt;
        if (f.dropped) recycle(ring, f);
      }, () => { f.failed = true; });
      ring.queued.push(f);
      return true;
    },

    // The oldest queued frame once mapped, leaving `keep` newer ones queued:
    // { data: ArrayBuffer (bytesPerRow per row), width, height, bytesPerRow,
    // bgra, latency: ms from readQueue until its map landed (as seen by the
    // event loop: the GPU finishing the frame and the copy) }, or null while its map has not landed (ask again after pumping
    // Node). "none" when there is no such frame, so the host does not wait
    // for nothing.
    readTake(id, keep = 0) {
      const ring = sessions.get(id)?.ring;
      if (!ring) return 'none';
      while (ring.queued.length && ring.queued[0].failed) ring.queued.shift();
      if (ring.queued.length <= keep) return 'none';
      const f = ring.queued[0];
      if (!f) return 'none';
      if (!f.ready) return null;
      ring.queued.shift();
      if (ring.taken) recycle(ring, ring.taken);
      ring.taken = f;
      return { data: f.buffer.getMappedRange(), width: f.width, height: f.height, bytesPerRow: f.bytesPerRow, bgra: f.bgra, latency: f.latency };
    },

    // The host is done with the frame readTake gave it.
    readRelease(id) {
      const ring = sessions.get(id)?.ring;
      if (ring?.taken) { recycle(ring, ring.taken); ring.taken = null; }
    },

    // A job: settles once the device is gone and the cart's pending callbacks
    // were answered. The host pumps it before tearing Node down.
    destroy(id) {
      const s = sessions.get(id);
      sessions.delete(id);
      return job(async () => {
        if (!s) return;
        try { s.surface?.destroy(); } catch {}
        if (s.ring) {
          for (const f of [...s.ring.free, ...s.ring.queued, ...(s.ring.taken ? [s.ring.taken] : [])]) {
            try { f.buffer.destroy(); } catch {}
          }
          s.ring = null;
        }
        await s.session.destroy();
        // Dawn rejects the cart's still-pending maps a couple of event-loop
        // turns AFTER device.lost (measured: 2 turns, every run). A promise
        // left unsettled when Node is torn down aborts the process, so give
        // it a bounded margin; each turn is a fraction of a millisecond.
        for (let i = 0; i < 20; i++) await new Promise(resolve => setImmediate(resolve));
        // The last cart gone: drop the instance too, so the host can collect
        // every Dawn object now, while Dawn is alive, instead of leaving them
        // to node::FreeEnvironment, which a player runs from a static
        // destructor at exit, after Dawn's own state may already be gone.
        if (sessions.size === 0) gpu = null;
      });
    },
  };
};
