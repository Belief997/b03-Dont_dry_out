/* live.js — the live-reload client. `core.js` owns the model merge; this
   file only adds the small niceties that depend on the dev server being
   present (rebuild-on-save indicator, last-built timestamp ticker). */
(function () {
  'use strict';
  const el = document.getElementById('live');
  if (!el) return;
  el.title = 'python build.py --serve 启动后会在此显示 live';
})();
