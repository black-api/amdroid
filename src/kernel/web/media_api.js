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

/* ---- fetch() ---- */
var fetches = {};
function Headers(h){ this.__h = {}; if(h) for(var k in h) this.__h[k.toLowerCase()] = String(h[k]); }
Headers.prototype.get = function(k){ var v = this.__h[String(k).toLowerCase()]; return v === undefined ? null : v; };
Headers.prototype.has = function(k){ return this.get(k) !== null; };
Headers.prototype.set = function(k, v){ this.__h[String(k).toLowerCase()] = String(v); };
function Response(buf, status, url){
  this.status = status || 200; this.ok = this.status >= 200 && this.status < 300;
  this.statusText = this.ok ? 'OK' : ''; this.url = url || ''; this.type = 'basic'; this.redirected = false;
  this.headers = new Headers(); this.bodyUsed = false; this.__b = buf;
}
Response.prototype.arrayBuffer = function(){ this.bodyUsed = true; return Promise.resolve(this.__b); };
Response.prototype.text = function(){ this.bodyUsed = true; return Promise.resolve(M.text(this.__b)); };
Response.prototype.json = function(){ this.bodyUsed = true; var t = M.text(this.__b); return new Promise(function(res, rej){ try { res(JSON.parse(t)); } catch(e){ rej(e); } }); };
Response.prototype.clone = function(){ return new Response(this.__b, this.status, this.url); };
w.Response = Response; w.Headers = Headers;
w.fetch = function(input, init){
  var url = String(input && input.url ? input.url : input);
  return new Promise(function(res, rej){
    var id = M.fetch(url);
    if(!id){ rej(new TypeError('Failed to fetch')); return; }
    fetches[id] = {res: res, rej: rej, url: url};
  });
};
/* the browser's answer (buf null: the request failed) */
w.__amdroidFetchDone = function(id, status, buf){
  var f = fetches[id];
  if(!f) return;
  delete fetches[id];
  if(buf === null){ f.rej(new TypeError('Failed to fetch (' + (status || 'network error') + ')')); return; }
  f.res(new Response(buf, status, f.url));
};

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
