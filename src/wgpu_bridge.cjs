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
  // Dawn instances by their flags (validation, a pinned adapter); dropped
  // with the last session
  const instances = new Map();
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
    // native: { register(ptr, handle), forget(ptr) } from cart_host.cpp, which
    // serves the cart's hot WebGPU imports itself (wgpu_imports.cpp): told the
    // native handle of every object those calls name, keyed by the cart's
    // pointer for it. Absent, every call goes through the glue.
    // validate: keep Dawn's command validation (a test run); a player's run
    // skips it ("skip_validation"), WASMCART_WGPU_VALIDATION=1 / 0 overriding.
    // pin: "vvvv:dddd", the PCI ids of the one GPU to use (the GPU of the
    // embedder's GL context), or empty. share: the device may share textures
    // with GL (Dawn's SharedTextureMemoryOpaqueFD and SharedFenceVkSemaphore
    // OpaqueFD, where the adapter has them: wc_host_wgpu_gl_share).
    prepare(id, module, width, height, native, validate, pin, share) {
      return job(async () => {
        if (process.env.WASMCART_NO_WGPU === '1') throw new Error('WebGPU is disabled (WASMCART_NO_WGPU=1)');
        const env = process.env.WASMCART_WGPU_VALIDATION;
        const on = env === '1' ? true : env === '0' ? false : !!validate;
        const flags = [...(on ? [] : ['enable-dawn-features=skip_validation']), ...(pin ? [`adapter-id=${pin}`] : [])];
        const key = flags.join(' ');
        if (!instances.has(key)) instances.set(key, dawn.create(flags));
        const gpu = instances.get(key);
        // WASMCART_WGPU_POWER=low-power|high-performance picks the GPU on a
        // two-GPU machine (wasmcart docs/webgpu.md); the choice is logged so a
        // caller can assert it. A pin names the GPU exactly instead.
        const adapterOptions = host.wgpuAdapterOptions();
        const adapter = await gpu.requestAdapter(adapterOptions).catch(e => { if (pin) return null; throw e; });
        if (!adapter) throw new Error(pin ? `no WebGPU adapter on the GPU ${pin} (the GL context's)` : 'no WebGPU adapter is available (check the GPU driver)');
        const a = host.describeAdapter(adapter, adapterOptions);
        process.stderr.write(`wasmcart-run: WebGPU on ${a.device || a.description} (${a.vendor}, ${a.featureLevel}${pin ? ', GPU ' + pin : a.powerPreference ? ', ' + a.powerPreference : ''}), Dawn validation ${on ? 'on' : 'off'}\n`);
        const shared = share ? dawn.addDeviceFeatures(adapter, ['SharedTextureMemoryOpaqueFD', 'SharedFenceVkSemaphoreOpaqueFD']) : [];
        const rec = { session: null, surface: null, size: [0, 0], lost: null, gpu, adapterObject: adapter,
          canShare: shared.length === 2,
          adapter: `${a.device || a.description} (${a.vendor}, ${a.featureLevel})` };
        let onObject = null, onRelease = null;
        if (native) {
          if (typeof dawn.objectHandle !== 'function') throw new Error(`${path.join(dir, 'dawn.node')} has no objectHandle (native-dawn 0.1.4 or newer is needed)`);
          // the objects the native imports name (passes, pipelines, bind
          // groups, buffers, queues, bundles); nothing else needs a handle
          const named = new Set(['GPURenderPassEncoder', 'GPUComputePassEncoder', 'GPURenderPipeline', 'GPUComputePipeline',
            'GPUBindGroup', 'GPUBuffer', 'GPUQueue', 'GPURenderBundle'].map(c => dawn.globals[c]));
          onObject = (ptr, obj) => {
            if (!named.has(obj?.constructor)) return;
            const h = dawn.objectHandle(obj);
            if (h !== undefined) native.register(ptr, h);
          };
          onRelease = ptr => native.forget(ptr);
        }
        rec.session = await host.createWgpuSession({
          moduleImports: WebAssembly.Module.imports(module), gpu, adapter, width, height,
          globals: dawn.globals,
          log: msg => process.stderr.write(msg + '\n'),
          onLost: info => { rec.lost = info; },
          onObject, onRelease,
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

    // Native handles for sharing the cart's frame with GL (wgpu_gl_share.cpp):
    // the adapter and device, and whether the device has the sharing features.
    shareHandles(id) {
      const s = sessions.get(id);
      if (!s?.session) return null;
      return { adapter: dawn.objectHandle(s.adapterObject), device: dawn.objectHandle(s.session.device), canShare: s.canShare };
    },

    // The cart's current frame: its texture's native handle, size and format;
    // null before the cart configured its canvas.
    frameTexture(id) {
      const session = sessions.get(id)?.session;
      if (!session) return null;
      let t;
      try { t = session.context.getCurrentTexture(); } catch { return null; }
      return { texture: dawn.objectHandle(t), width: t.width, height: t.height, format: t.format };
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

    // After wc_render: the frame's last submit is done (wasmcart destroys the
    // textures and buffers the cart released then).
    end(id) {
      sessions.get(id)?.session.endFrame();
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
    //
    // When the cart's canvas is the window's size and fills it, the cart
    // draws straight into the window's texture from the next frame on (the
    // surface configured as the cart configured its canvas): no copy into the
    // window, no extra pass. Any other size, or a surface that cannot take the
    // cart's format and usage, letterboxes with a copy.
    present(id, x, y, w, h, winW, winH) {
      const s = sessions.get(id);
      if (!s?.surface) return false;
      const canvas = s.session.canvas;
      if (canvas.isDirect) {  // this frame was drawn into the window's texture
        s.surface.present();
        return true;
      }
      const cfg = s.session.context.getConfiguration?.();
      const fills = x === 0 && y === 0 && w === winW && h === winH && canvas.width === winW && canvas.height === winH;
      const directKey = fills && cfg ? `${winW}x${winH} ${cfg.format} ${cfg.usage} ${(cfg.viewFormats || []).join()}` : '';
      // drawing straight into the window, and the cart drew nothing this frame: the window keeps its last one
      if (s.direct && s.direct === directKey && canvas.hasDirect) return false;
      if (s.direct !== directKey || s.size[0] !== winW || s.size[1] !== winH) {
        s.direct = '';
        canvas.setDirect?.(null);
        // The first present mode the surface supports (configure checks).
        const configure = (format, usage, viewFormats) => {
          let err;
          for (const presentMode of s.presentModes) {
            try {
              s.surface.configure({ width: winW, height: winH, format, usage, viewFormats, presentMode });
              return null;
            } catch (e) { err = e; }
          }
          return err;
        };
        if (directKey && !s.directRefused?.has(directKey) && !configure(cfg.format, cfg.usage, cfg.viewFormats || [])) {
          s.direct = directKey;
        } else {
          if (directKey) (s.directRefused ??= new Set()).add(directKey);
          const err = configure(s.gpu.getPreferredCanvasFormat(), 0x10, []);
          if (err) throw err;
        }
        s.size = [winW, winH];
      }
      const texture = s.surface.getCurrentTexture();
      const drew = s.session.drawTo({ getCurrentTexture: () => texture }, { x, y, w, h });
      s.surface.present();
      // the surface now matches the canvas: the cart's next frame goes straight into it
      if (s.direct && canvas.setDirect) canvas.setDirect(() => s.surface.getCurrentTexture());
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
      let data;
      // Throws once the device is gone (lost or destroyed after the map landed).
      try { data = f.buffer.getMappedRange(); } catch { return 'none'; }
      ring.taken = f;
      return { data, width: f.width, height: f.height, bytesPerRow: f.bytesPerRow, bgra: f.bgra, latency: f.latency };
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
        if (sessions.size === 0) instances.clear();
      });
    },
  };
};
