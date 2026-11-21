/* media_api.js — fetch(), Media Source Extensions and <video>, for pages in the am-droid
 * browser. Runs after browser_api.inc, before the page's scripts.
 *
 * The objects follow the web platform (MediaSource, SourceBuffer, TimeRanges,
 * HTMLVideoElement, fetch/Response), so a page drives them the way it would in any
 * browser. Underneath, __media (native, media_api.inc) passes what they do to the browser
 * app, which feeds Reel, the OS's media process: it demuxes what is appended, decodes it
 * (H.264, VP9, AV1, AAC, Opus) and plays it, and the picture is drawn into the element's
 * box. The state comes back about ten times a second through __amdroidMediaTick, which
 * fires the events (updateend, timeupdate, seeked, ended...).
 *
 * Generated into media_api.inc by tools/js2inc.py. */
(function(w){
var M = w.__media;
if(!M) return;
function noop(){}

/* ---- events for objects that are not elements ---- */
function ET(){}
ET.prototype.addEventListener = function(t, f){ if(typeof f !== 'function') return; var l = this.__l || (this.__l = {}); (l[t] || (l[t] = [])).push(f); };
ET.prototype.removeEventListener = function(t, f){ var a = this.__l && this.__l[t]; if(a) this.__l[t] = a.filter(function(x){ return x !== f; }); };
ET.prototype.dispatchEvent = function(e){
  var a = ((this.__l && this.__l[e.type]) || []).slice();
  for(var i = 0; i < a.length; i++){ try { a[i].call(this, e); } catch(x){ console.log('media event ' + e.type + ': ' + x); } }
  var h = this['on' + e.type];
  if(typeof h === 'function'){ try { h.call(this, e); } catch(x){ console.log('media event ' + e.type + ': ' + x); } }
  return true;
};
function fire(o, type){ o.dispatchEvent({type: type, target: o, currentTarget: o, preventDefault: noop, stopPropagation: noop}); }
/* elements: through the native dispatcher, so addEventListener and onX both work */
function fireEl(el, type){ M.dispatch(el, type); }

/* ---- fetch() and XMLHttpRequest ----
 * Both go through M.fetch(url, method, body, contentType): the browser app sends the request
 * (with the site's cookies) and the answer comes back through __amdroidFetchDone. A body is
 * a string or bytes (up to ~12 KB). Answers with any HTTP status reach the page. */
var fetches = {};
function Headers(h){
  this.__h = {};
  if(!h) return;
  if(h instanceof Headers){ for(var k in h.__h) this.__h[k] = h.__h[k]; }
  else if(Array.isArray(h)){ for(var i = 0; i < h.length; i++) this.__h[String(h[i][0]).toLowerCase()] = String(h[i][1]); }
  else for(var k2 in h) this.__h[k2.toLowerCase()] = String(h[k2]);
}
Headers.prototype.get = function(k){ var v = this.__h[String(k).toLowerCase()]; return v === undefined ? null : v; };
Headers.prototype.has = function(k){ return this.get(k) !== null; };
Headers.prototype.set = Headers.prototype.append = function(k, v){ this.__h[String(k).toLowerCase()] = String(v); };
Headers.prototype['delete'] = function(k){ delete this.__h[String(k).toLowerCase()]; };
Headers.prototype.forEach = function(f, t){ for(var k in this.__h) f.call(t, this.__h[k], k, this); };
Headers.prototype.entries = function(){ var a = []; for(var k in this.__h) a.push([k, this.__h[k]]); return a[Symbol.iterator](); };
Headers.prototype.keys = function(){ return Object.keys(this.__h)[Symbol.iterator](); };
Headers.prototype[Symbol.iterator] = Headers.prototype.entries;
var STATUS = {200: 'OK', 201: 'Created', 204: 'No Content', 206: 'Partial Content', 301: 'Moved Permanently', 302: 'Found',
  304: 'Not Modified', 400: 'Bad Request', 401: 'Unauthorized', 403: 'Forbidden', 404: 'Not Found', 405: 'Method Not Allowed',
  429: 'Too Many Requests', 500: 'Internal Server Error', 502: 'Bad Gateway', 503: 'Service Unavailable'};
function Response(buf, status, url){
  if(!(buf instanceof ArrayBuffer) && buf !== undefined && buf !== null && typeof status !== 'number'){
    /* new Response(body, init) from a page script */
    var init = status || {}; status = init.status || 200;
    buf = typeof buf === 'string' ? new TextEncoder().encode(buf).buffer : buf;
    this.headers = new Headers(init.headers);
  }
  this.status = status === undefined ? 200 : status; this.ok = this.status >= 200 && this.status < 300;
  this.statusText = STATUS[this.status] || ''; this.url = url || ''; this.type = 'basic'; this.redirected = false;
  if(!this.headers) this.headers = new Headers(); this.bodyUsed = false; this.__b = buf || new ArrayBuffer(0);
}
Response.prototype.arrayBuffer = function(){ this.bodyUsed = true; return Promise.resolve(this.__b); };
Response.prototype.text = function(){ this.bodyUsed = true; return Promise.resolve(M.text(this.__b)); };
Response.prototype.json = function(){ this.bodyUsed = true; var t = M.text(this.__b); return new Promise(function(res, rej){ try { res(JSON.parse(t)); } catch(e){ rej(e); } }); };
Response.prototype.blob = function(){ this.bodyUsed = true; var b = this.__b; return Promise.resolve(w.Blob ? new w.Blob([b]) : b); };
Response.prototype.clone = function(){ return new Response(this.__b, this.status, this.url); };
function Request(u, init){
  init = init || {};
  if(u instanceof Request){ this.url = u.url; this.method = init.method || u.method; this.body = init.body !== undefined ? init.body : u.body; this.headers = new Headers(init.headers || u.headers); }
  else { this.url = String(u); this.method = init.method || 'GET'; this.body = init.body; this.headers = new Headers(init.headers); }
  this.method = String(this.method).toUpperCase(); this.credentials = init.credentials || 'same-origin'; this.signal = init.signal || null;
}
Request.prototype.clone = function(){ return new Request(this); };
w.Response = Response; w.Headers = Headers; w.Request = Request;

/* a body as a string or bytes, and the type it implies */
function bodyOf(b, ctype){
  if(b === undefined || b === null) return [null, ctype];
  if(typeof b === 'string') return [b, ctype || 'text/plain;charset=UTF-8'];
  if(w.URLSearchParams && b instanceof w.URLSearchParams) return [b.toString(), ctype || 'application/x-www-form-urlencoded;charset=UTF-8'];
  if(w.FormData && b instanceof w.FormData){
    /* sent url-encoded (no files here) */
    var parts = [];
    if(b.forEach) b.forEach(function(v, k){ parts.push(encodeURIComponent(k) + '=' + encodeURIComponent(v)); });
    return [parts.join('&'), ctype || 'application/x-www-form-urlencoded;charset=UTF-8'];
  }
  if(b instanceof ArrayBuffer || ArrayBuffer.isView(b)) return [b, ctype || ''];
  if(w.Blob && b instanceof w.Blob && b.__parts) return [b.__parts.join(''), ctype || b.type || ''];
  return [String(b), ctype || 'text/plain;charset=UTF-8'];
}

function abortErr(){ var e = new Error('The operation was aborted.'); e.name = 'AbortError'; return e; }
w.fetch = function(input, init){
  var rq = new Request(input, init);
  return new Promise(function(res, rej){
    if(rq.signal && rq.signal.aborted){ rej(abortErr()); return; }
    var bc = bodyOf(rq.method === 'GET' || rq.method === 'HEAD' ? null : rq.body, rq.headers.get('content-type'));
    var id = M.fetch(rq.url, rq.method, bc[0], bc[1] || '');
    if(!id){ rej(new TypeError('Failed to fetch')); return; }
    fetches[id] = {res: res, rej: rej, url: rq.url};
    if(rq.signal && rq.signal.addEventListener) rq.signal.addEventListener('abort', function(){
      if(fetches[id]){ delete fetches[id]; rej(abortErr()); } });
  });
};

function XMLHttpRequest(){
  this.readyState = 0; this.status = 0; this.statusText = ''; this.responseText = ''; this.response = '';
  this.responseType = ''; this.responseURL = ''; this.responseXML = null; this.timeout = 0; this.withCredentials = false;
  this.upload = new ET(); this.__h = {};
}
XMLHttpRequest.prototype = Object.create(ET.prototype);
XMLHttpRequest.prototype.constructor = XMLHttpRequest;
XMLHttpRequest.UNSENT = 0; XMLHttpRequest.OPENED = 1; XMLHttpRequest.HEADERS_RECEIVED = 2; XMLHttpRequest.LOADING = 3; XMLHttpRequest.DONE = 4;
['UNSENT', 'OPENED', 'HEADERS_RECEIVED', 'LOADING', 'DONE'].forEach(function(k, i){ XMLHttpRequest.prototype[k] = i; });
function xfire(x, type, extra){
  var e = {type: type, target: x, currentTarget: x, preventDefault: noop, stopPropagation: noop, lengthComputable: false, loaded: 0, total: 0};
  if(extra) for(var k in extra) e[k] = extra[k];
  x.dispatchEvent(e);
}
XMLHttpRequest.prototype.open = function(m, u){
  this.__m = String(m || 'GET').toUpperCase(); this.__u = String(u); this.__h = {}; this.__id = 0;
  this.readyState = 1; this.status = 0; this.responseText = ''; this.response = '';
  xfire(this, 'readystatechange');
};
XMLHttpRequest.prototype.setRequestHeader = function(k, v){ this.__h[String(k).toLowerCase()] = String(v); };
XMLHttpRequest.prototype.overrideMimeType = noop;
XMLHttpRequest.prototype.getResponseHeader = function(k){ return String(k).toLowerCase() === 'content-type' && this.__ct ? this.__ct : null; };
XMLHttpRequest.prototype.getAllResponseHeaders = function(){ return this.__ct ? 'content-type: ' + this.__ct + '\r\n' : ''; };
XMLHttpRequest.prototype.abort = function(){
  if(this.__id && fetches[this.__id]){ delete fetches[this.__id]; this.__id = 0; this.readyState = 4; xfire(this, 'readystatechange'); xfire(this, 'abort'); xfire(this, 'loadend'); }
  this.readyState = 0;
};
XMLHttpRequest.prototype.send = function(body){
  var x = this;
  if(x.readyState !== 1) throw new Error('InvalidStateError: open() first');
  var bc = bodyOf(x.__m === 'GET' || x.__m === 'HEAD' ? null : body, x.__h['content-type']);
  var id = M.fetch(x.__u, x.__m, bc[0], bc[1] || '');
  xfire(x, 'loadstart');
  if(!id){ setTimeout(function(){ x.readyState = 4; xfire(x, 'readystatechange'); xfire(x, 'error'); xfire(x, 'loadend'); }, 0); return; }
  x.__id = id;
  fetches[id] = {xhr: x, url: x.__u};
  if(x.timeout > 0) setTimeout(function(){
    if(fetches[id]){ delete fetches[id]; x.readyState = 4; xfire(x, 'readystatechange'); xfire(x, 'timeout'); xfire(x, 'loadend'); } }, x.timeout);
};
function xhrDone(x, status, buf, url){
  x.__id = 0;
  if(buf === null){ x.readyState = 4; x.status = 0; xfire(x, 'readystatechange'); xfire(x, 'error'); xfire(x, 'loadend'); return; }
  x.status = status; x.statusText = STATUS[status] || ''; x.responseURL = url;
  x.readyState = 2; xfire(x, 'readystatechange');
  x.readyState = 3; xfire(x, 'readystatechange');
  var t = x.responseType;
  if(t === 'arraybuffer') x.response = buf;
  else if(t === 'blob') x.response = w.Blob ? new w.Blob([buf]) : buf;
  else {
    var text = M.text(buf);
    x.responseText = text;
    if(t === 'json'){ try { x.response = JSON.parse(text); } catch(e){ x.response = null; } }
    else if(t === 'document') x.response = null;
    else x.response = text;
  }
  x.readyState = 4;
  xfire(x, 'readystatechange');
  xfire(x, 'progress', {loaded: buf.byteLength, total: buf.byteLength, lengthComputable: true});
  xfire(x, 'load', {loaded: buf.byteLength, total: buf.byteLength});
  xfire(x, 'loadend');
}
w.XMLHttpRequest = XMLHttpRequest;

/* the browser's answer (buf null: the request failed) */
w.__amdroidFetchDone = function(id, status, buf){
  var f = fetches[id];
  if(!f) return;
  delete fetches[id];
  if(f.xhr){ xhrDone(f.xhr, status, buf, f.url); return; }
  if(f.img){ M.dispatch(f.img, buf !== null && status < 400 ? 'load' : 'error'); return; }
  if(buf === null){ f.rej(new TypeError('Failed to fetch (' + (status || 'network error') + ')')); return; }
  f.res(new Response(buf, status, f.url));
};

/* new Image(): a detached <img> loads when given a src (preloading, beacons) and fires
 * load or error. Images in the page load with the page as before. */
var imgs = [], imgTimer = 0;
function watchImgs(){
  imgTimer = 0;
  for(var i = imgs.length - 1; i >= 0; i--){
    var e = imgs[i], s = e.el.getAttribute && e.el.getAttribute('src');
    if(s){ imgs.splice(i, 1); var id = M.fetch(s, 'GET', null, ''); if(id) fetches[id] = {img: e.el, url: s}; else M.dispatch(e.el, 'error'); }
    else if(++e.n > 200) imgs.splice(i, 1);
  }
  if(imgs.length) imgTimer = setTimeout(watchImgs, 50);
}
w.Image = function(wd, ht){
  var el = w.document.createElement('img');
  if(wd !== undefined) el.setAttribute('width', String(wd));
  if(ht !== undefined) el.setAttribute('height', String(ht));
  imgs.push({el: el, n: 0});
  if(!imgTimer) imgTimer = setTimeout(watchImgs, 0);
  return el;
};
w.Image.prototype = w.HTMLImageElement ? w.HTMLImageElement.prototype : {};

/* ---- TimeRanges ---- */
function TimeRanges(a){ this.__a = a || []; this.length = this.__a.length / 2; }
TimeRanges.prototype.start = function(i){ if(i < 0 || i >= this.length) throw new RangeError('IndexSizeError'); return this.__a[i * 2]; };
TimeRanges.prototype.end = function(i){ if(i < 0 || i >= this.length) throw new RangeError('IndexSizeError'); return this.__a[i * 2 + 1]; };
w.TimeRanges = TimeRanges;
function ranges(us){ return new TimeRanges((us || []).map(function(x){ return x / 1e6; })); }

/* the last state the browser reported */
var st = null;

/* ---- MediaSource ---- */
/* what Reel decodes: H.264, VP9 profile 0, AV1 8-bit; AAC, Opus. Not 10-bit or HDR. */
function codecOk(c){
  c = c.trim();
  if(/^(avc1|avc3)\./.test(c) || c === 'avc1') return true;
  if(c === 'vp9' || c === 'vp09' || /^vp09\.00\./.test(c)) return true;
  if(/^av01\.0\.\d\d[MH]\.08/.test(c) || c === 'av01') return true;
  if(/^mp4a\.40\.(2|5|29)$/.test(c) || c === 'mp4a' || c === 'opus') return true;
  return false;
}
function typeOk(t){
  t = String(t);
  var m = /^\s*(video|audio)\/(mp4|webm)\s*(;|$)/i.exec(t);
  if(!m) return false;
  var c = /codecs\s*=\s*"?([^"]*)"?/i.exec(t);
  if(!c) return true;
  return c[1].split(',').every(codecOk);
}
function MediaSource(){
  this.readyState = 'closed';
  this.sourceBuffers = []; this.activeSourceBuffers = this.sourceBuffers;
  this.__dur = NaN; this.__video = null;
}
MediaSource.prototype = Object.create(ET.prototype);
MediaSource.prototype.constructor = MediaSource;
MediaSource.isTypeSupported = typeOk;
MediaSource.prototype.addSourceBuffer = function(type){
  if(this.readyState !== 'open') throw new Error('InvalidStateError: the MediaSource is not open');
  if(!typeOk(type)) throw new Error('NotSupportedError: ' + type);
  if(this.sourceBuffers.length >= 2) throw new Error('QuotaExceededError: two source buffers at most');
  var sb = new SourceBuffer(this, this.sourceBuffers.length, type);
  this.sourceBuffers.push(sb);
  return sb;
};
MediaSource.prototype.removeSourceBuffer = function(sb){ var i = this.sourceBuffers.indexOf(sb); if(i >= 0) this.sourceBuffers.splice(i, 1); };
MediaSource.prototype.endOfStream = function(){
  if(this.readyState !== 'open') return;
  this.readyState = 'ended';
  M.cmd(5, 0, 0, 0);
  fire(this, 'sourceended');
};
MediaSource.prototype.setLiveSeekableRange = noop;
MediaSource.prototype.clearLiveSeekableRange = noop;
Object.defineProperty(MediaSource.prototype, 'duration', {
  get: function(){ return this.__dur; },
  set: function(v){ this.__dur = +v; if(this.__video) fireEl(this.__video, 'durationchange'); }
});
w.MediaSource = MediaSource; w.WebKitMediaSource = MediaSource;

/* ---- SourceBuffer ---- */
function SourceBuffer(ms, idx, type){
  this.__ms = ms; this.__i = idx; this.type = type;
  this.updating = false; this.mode = 'segments'; this.timestampOffset = 0;
  this.appendWindowStart = 0; this.appendWindowEnd = Infinity;
}
SourceBuffer.prototype = Object.create(ET.prototype);
SourceBuffer.prototype.constructor = SourceBuffer;
Object.defineProperty(SourceBuffer.prototype, 'buffered', {
  get: function(){ return ranges(st && st.ranges[this.__i]); }
});
function startUpdate(sb){ sb.updating = true; sb.__since = M.sent(sb.__i); fire(sb, 'updatestart'); }
SourceBuffer.prototype.appendBuffer = function(data){
  if(this.updating) throw new Error('InvalidStateError: still updating');
  if(this.__ms.readyState === 'closed') throw new Error('InvalidStateError: the MediaSource is closed');
  if(this.__ms.readyState === 'ended'){ this.__ms.readyState = 'open'; fire(this.__ms, 'sourceopen'); }
  if(!M.append(this.__i, data)) throw new Error('QuotaExceededError: the data could not be queued');
  startUpdate(this);
};
SourceBuffer.prototype.remove = function(a, b){
  if(this.updating) throw new Error('InvalidStateError: still updating');
  M.cmd(3, this.__i, a * 1e6, b * 1e6);
  startUpdate(this);
};
SourceBuffer.prototype.abort = function(){
  M.cmd(4, this.__i, 0, 0);
  if(this.updating){ this.updating = false; fire(this, 'abort'); fire(this, 'updateend'); }
};
SourceBuffer.prototype.changeType = function(t){ if(!typeOk(t)) throw new Error('NotSupportedError: ' + t); this.type = t; };
w.SourceBuffer = SourceBuffer;

/* ---- object URLs: a MediaSource becomes a video's src ---- */
var objs = {}, nextObj = 1;
var oldCreate = w.URL.createObjectURL;
w.URL.createObjectURL = function(o){
  if(o instanceof MediaSource){ var u = 'blob:' + (w.location.origin || 'null') + '/mediasource-' + (nextObj++); objs[u] = o; return u; }
  return oldCreate ? oldCreate(o) : 'blob:null';
};
w.URL.revokeObjectURL = function(u){ delete objs[u]; };

/* ---- <video> (and <audio>) ---- */
var cur = null;                       /* the element playing (one at a time) */
function V(el){
  return el.__v || (el.__v = {src: '', ms: null, paused: true, ended: false, seeking: false, target: 0,
                               volume: 1, muted: false, error: null, ready: 0, lastT: -1, lastFireT: -1,
                               waiting: false, errSeq: -1, dur: NaN});
}
function detach(el){
  var v = V(el);
  if(v.ms){ v.ms.readyState = 'closed'; v.ms.__video = null; fire(v.ms, 'sourceclose'); }
  v.ms = null;
  if(cur === el){ M.cmd(9, 0, 0, 0); cur = null; }
}
var VP = (w.HTMLVideoElement && w.HTMLVideoElement.prototype) || {};
function prop(name, get, set){ Object.defineProperty(VP, name, {get: get, set: set || noop, configurable: true}); }
prop('src', function(){ return V(this).src; }, function(u){
  var el = this, v = V(el);
  u = String(u);
  detach(el);
  v.src = u; v.paused = true; v.ended = false; v.error = null; v.ready = 0; v.lastT = -1; v.dur = NaN;
  var ms = objs[u];
  if(!ms){
    if(u){ v.error = {code: 4, message: 'only MediaSource object URLs can be played'}; setTimeout(function(){ fireEl(el, 'error'); }, 0); }
    return;
  }
  if(cur && cur !== el) detach(cur);
  cur = el;
  v.ms = ms; ms.__video = el;
  M.cmd(1, 0, 0, 0);
  ms.readyState = 'open';
  setTimeout(function(){ fireEl(el, 'loadstart'); fire(ms, 'sourceopen'); }, 0);
});
prop('currentSrc', function(){ return V(this).src; });
prop('srcObject', function(){ return V(this).ms; }, function(ms){ if(ms instanceof MediaSource){ var u = w.URL.createObjectURL(ms); this.src = u; } });
prop('currentTime', function(){
  var v = V(this);
  if(v.seeking) return v.target;
  return st && cur === this ? st.t / 1e6 : 0;
}, function(t){
  var v = V(this);
  t = +t; if(!(t >= 0)) t = 0;
  v.target = t; v.seeking = true; v.ended = false;
  if(cur === this) M.cmd(8, 0, t * 1e6, 0);
  fireEl(this, 'seeking');
});
prop('duration', function(){
  var v = V(this);
  if(v.ms && v.ms.__dur === v.ms.__dur) return v.ms.__dur;
  return st && cur === this && st.dur > 0 ? st.dur / 1e6 : NaN;
});
prop('paused', function(){ return V(this).paused; });
prop('ended', function(){ return V(this).ended; });
prop('seeking', function(){ return V(this).seeking; });
prop('readyState', function(){ return V(this).ready; });
prop('networkState', function(){ return V(this).ms ? 2 : 0; });
prop('error', function(){ return V(this).error; });
prop('videoWidth', function(){ return st && cur === this ? st.vw : 0; });
prop('videoHeight', function(){ return st && cur === this ? st.vh : 0; });
prop('buffered', function(){
  /* what every source buffer has (the intersection), as MSE defines it */
  var v = V(this);
  if(!v.ms || !st) return new TimeRanges([]);
  var sbs = v.ms.sourceBuffers, out = null;
  for(var i = 0; i < sbs.length; i++){
    var r = (st.ranges[sbs[i].__i] || []).map(function(x){ return x / 1e6; });
    if(out === null){ out = r; continue; }
    var x = [];
    for(var a = 0; a < out.length; a += 2) for(var b = 0; b < r.length; b += 2){
      var s = Math.max(out[a], r[b]), e = Math.min(out[a + 1], r[b + 1]);
      if(e > s) x.push(s, e);
    }
    out = x;
  }
  return new TimeRanges(out || []);
});
prop('seekable', function(){ var d = this.duration; return new TimeRanges(d === d ? [0, d] : []); });
prop('played', function(){ return new TimeRanges([]); });
prop('volume', function(){ return V(this).volume; }, function(x){
  var v = V(this); v.volume = Math.max(0, Math.min(1, +x || 0));
  if(cur === this) M.cmd(10, v.muted ? 0 : Math.round(v.volume * 100), 0, 0);
  fireEl(this, 'volumechange');
});
prop('muted', function(){ return V(this).muted; }, function(x){
  var v = V(this); v.muted = !!x;
  if(cur === this) M.cmd(10, v.muted ? 0 : Math.round(v.volume * 100), 0, 0);
  fireEl(this, 'volumechange');
});
prop('playbackRate', function(){ return 1; });
prop('defaultPlaybackRate', function(){ return 1; });
prop('autoplay', function(){ return this.hasAttribute ? this.hasAttribute('autoplay') : false; });
prop('loop', function(){ return false; });
prop('controls', function(){ return false; });
prop('preload', function(){ return 'auto'; });
VP.play = function(){
  var el = this, v = V(el);
  if(!v.ms) return Promise.reject(new Error('NotSupportedError: no source'));
  if(cur !== el){ cur = el; }
  if(v.ended){ v.ended = false; el.currentTime = 0; }
  if(v.paused){ v.paused = false; M.cmd(6, 0, 0, 0); fireEl(el, 'play'); }
  return Promise.resolve();
};
VP.pause = function(){
  var v = V(this);
  if(!v.paused){ v.paused = true; if(cur === this) M.cmd(7, 0, 0, 0); fireEl(this, 'pause'); }
};
VP.load = function(){ var s = V(this).src; if(s) this.src = s; };
VP.canPlayType = function(t){ return typeOk(t) ? 'probably' : ''; };
VP.fastSeek = function(t){ this.currentTime = t; };
VP.getVideoPlaybackQuality = function(){ return {totalVideoFrames: st ? st.shown : 0, droppedVideoFrames: 0, corruptedVideoFrames: 0}; };
if(w.HTMLAudioElement) w.HTMLAudioElement.prototype = VP;

/* ---- the state, about ten times a second, while a presentation is open ---- */
w.__amdroidMediaTick = function(s){
  st = s;
  var el = cur;
  if(!el) return;
  var v = V(el), ms = v.ms;
  /* source buffers whose work is done */
  if(ms) ms.sourceBuffers.forEach(function(sb){
    if(sb.updating && M.done(sb.__i, sb.__since)){ sb.updating = false; fire(sb, 'update'); fire(sb, 'updateend'); }
  });
  if(s.errSeq !== v.errSeq){
    if(v.errSeq !== -1 && s.err){ v.error = {code: 3, message: s.err}; fireEl(el, 'error'); }
    v.errSeq = s.errSeq;
  }
  /* readiness: metadata once something is known, enough data once a picture (or sound) is ready */
  if(v.ready < 1 && (s.vw > 0 || s.dur > 0)){ v.ready = 1; fireEl(el, 'loadedmetadata'); fireEl(el, 'durationchange'); }
  if(v.ready < 2 && s.ready){ v.ready = 4; fireEl(el, 'loadeddata'); fireEl(el, 'canplay'); fireEl(el, 'canplaythrough'); }
  if(v.seeking && s.ready && Math.abs(s.t / 1e6 - v.target) < 1.5){ v.seeking = false; fireEl(el, 'seeked'); fireEl(el, 'timeupdate'); }
  if(!v.paused){
    if(s.stalled && !v.waiting){ v.waiting = true; fireEl(el, 'waiting'); }
    if(!s.stalled && s.playing && (v.waiting || v.lastT < 0)){ v.waiting = false; fireEl(el, 'playing'); }
  }
  var t = s.t / 1e6;
  if(!v.seeking && Math.abs(t - v.lastFireT) >= 0.25){ v.lastFireT = t; fireEl(el, 'timeupdate'); }
  v.lastT = t;
  if(s.ended && !v.ended && !v.paused){ v.ended = true; v.paused = true; fireEl(el, 'pause'); fireEl(el, 'ended'); }
};
})(this);
