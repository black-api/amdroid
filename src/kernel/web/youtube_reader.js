(function(w){
var d = w.document, data = w.ytInitialData || {}, player = w.ytInitialPlayerResponse || null;
function txt(t){ if(!t) return ''; if(typeof t === 'string') return t; if(t.simpleText) return t.simpleText; if(t.content) return t.content;
  if(t.runs) return t.runs.map(function(r){ return r.text; }).join(''); return ''; }
function esc(s){ return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;'); }
function thumb(id, big){ return 'https://i.ytimg.com/vi/' + id + (big ? '/hqdefault.jpg' : '/mqdefault.jpg'); }
function num(n){ n = +n; if(!(n >= 0)) return ''; if(n >= 1e9) return (n / 1e9).toFixed(1) + 'B'; if(n >= 1e6) return (n / 1e6).toFixed(1) + 'M'; if(n >= 1e3) return (n / 1e3).toFixed(1) + 'K'; return String(n); }
/* every video in the page data: search results (videoRenderer) and related videos (lockupViewModel) */
var vids = [], seen = {};
function walk(o, depth){
  if(!o || typeof o !== 'object' || depth > 60 || vids.length >= 40) return;
  if(Array.isArray(o)){ for(var i = 0; i < o.length; i++) walk(o[i], depth + 1); return; }
  var r = o.videoRenderer || o.compactVideoRenderer || o.gridVideoRenderer;
  if(r && r.videoId && !seen[r.videoId]){
    seen[r.videoId] = 1;
    vids.push({id: r.videoId, title: txt(r.title), channel: txt(r.ownerText || r.longBylineText || r.shortBylineText),
               views: txt(r.viewCountText || r.shortViewCountText), age: txt(r.publishedTimeText), len: txt(r.lengthText)});
    return;
  }
  var l = o.lockupViewModel;
  if(l && l.contentId && String(l.contentType).indexOf('VIDEO') >= 0 && !seen[l.contentId]){
    seen[l.contentId] = 1;
    var md = l.metadata && l.metadata.lockupMetadataViewModel, rows = [], len = '';
    try { rows = md.metadata.contentMetadataViewModel.metadataRows; } catch(e){}
    try { var ov = l.contentImage.thumbnailViewModel.overlays;
          for(var j = 0; j < ov.length; j++) if(ov[j].thumbnailBottomOverlayViewModel) len = ov[j].thumbnailBottomOverlayViewModel.badges[0].thumbnailBadgeViewModel.text; } catch(e){}
    function row(k){ return rows[k] ? rows[k].metadataParts.map(function(p){ return txt(p.text); }).join(' - ') : ''; }
    vids.push({id: l.contentId, title: md ? txt(md.title) : '', channel: row(0), views: row(1), age: '', len: len});
    return;
  }
  for(var k in o) walk(o[k], depth + 1);
}
var path = w.location.pathname || '/', search = w.location.search || '';
var query = ''; var m = /[?&]search_query=([^&]*)/.exec(search); if(m) query = decodeURIComponent(m[1].replace(/\+/g, ' '));
var C = {bg: '#0f0f0f', fg: '#f1f1f1', dim: '#aaaaaa', red: '#ff0000', box: '#272727'};
var html = '';
/* the header: logo and search */
html += '<table width="100%" style="background:' + C.bg + '"><tr><td width="260">' +
        '<span style="background:' + C.red + ';color:#ffffff;font-weight:bold;padding:2px 10px">&gt;</span> ' +
        '<a href="/" style="color:' + C.fg + ';font-weight:bold;font-size:24px">YouTube</a></td>' +
        '<td><input id="q" placeholder="Search" value="' + esc(query) + '"> <input type="button" id="go" value="Search"></td>' +
        '<td width="300" style="color:' + C.dim + '">read by the am-droid browser</td></tr></table>';
/* one table per list, one row per video: the columns line up */
function list(vs, w, h){
  return '<table width="100%">' + vs.map(function(v){
    return '<tr><td width="' + (w + 16) + '" valign="top" style="padding-bottom:12px">' +
      '<a href="/watch?v=' + v.id + '"><img src="' + thumb(v.id) + '" width="' + w + '" height="' + h + '" alt="' + esc(v.title) + '"></a>' +
      (v.len ? '<br><span style="background:#000000;color:#ffffff;padding:0 4px">' + esc(v.len) + '</span>' : '') +
      '</td><td valign="top"><a href="/watch?v=' + v.id + '" style="color:' + C.fg + ';font-weight:bold">' + esc(v.title) + '</a><br>' +
      '<span style="color:' + C.dim + '">' + esc(v.channel) + '</span><br>' +
      '<span style="color:' + C.dim + '">' + esc([v.views, v.age].filter(function(s){ return s; }).join(' - ')) + '</span></td></tr>';
  }).join('') + '</table>';
}
function playerHtml(id, alt){
  /* as on YouTube: the video, and under it one control row (play/pause, the seek bar, the time) */
  /* the player fills its column (the video is 16:9 of the column's width) */
  return '<table id="player" width="100%"><tr><td valign="top"><video id="vid" style="width:100%"></video>' +
    '<table width="100%" style="margin-top:4px"><tr><td id="play" width="28" style="color:' + C.fg + ';font-size:18px;text-align:center">&#9654;</td>' +
    '<td><table width="100%"><tr id="vbar">' + bar() + '</tr></table></td>' +
    '<td width="150" id="vtime" style="color:' + C.fg + ';text-align:right">0:00</td></tr></table>' +
    '<div id="vst" style="color:' + C.dim + ';margin-top:2px;font-size:12px">Loading the video...</div></td></tr></table>';
}
/* the seek bar: cells to click (a click carries no position here) */
var BAR = 48;
function bar(){ var h = ''; for(var i = 0; i < BAR; i++) h += '<td id="vb' + i + '" style="background:#3a3a3a;font-size:6px">&nbsp;</td>'; return h; }
var title = 'YouTube';
/* /shorts/<id> is a video too, and /watch sometimes arrives without its player data */
var shortId = (/^\/shorts\/([\w-]{6,})/.exec(path) || [])[1] || '';
var vd = ((path === '/watch' || shortId) && player && player.videoDetails) || null;
/* the address carries the id even when YouTube sends no player data for the page */
var urlVid = (/[?&]v=([\w-]{6,})/.exec(search) || [])[1] || '';
var vid = shortId || urlVid;
if(vd){
  vid = vd.videoId || vid;
  title = vd.title + ' - YouTube';
  var date = ''; (function find(o, depth){ if(!o || typeof o !== 'object' || depth > 30 || date) return;
    if(o.videoPrimaryInfoRenderer){ date = txt(o.videoPrimaryInfoRenderer.dateText); return; } for(var k in o) find(o[k], depth + 1); })(data, 0);
  walk(data, 0);
  /* two columns, as on YouTube: the video and its details, and the videos up next */
  html += '<table width="100%"><tr><td valign="top" width="64%">' +
    playerHtml(vid, vd.title) +
    '<h2 style="color:' + C.fg + '">' + esc(vd.title) + '</h2>' +
    '<p style="color:' + C.fg + '"><b>' + esc(vd.author) + '</b></p>' +
    '<p style="color:' + C.dim + '">' + num(vd.viewCount) + ' views' + (date ? ' - ' + esc(date) : '') + '</p>' +
    '<p style="color:' + C.fg + ';background:' + C.box + ';padding:8px">' + esc(vd.shortDescription || '').replace(/\n/g, '<br>') + '</p>' +
    '</td><td width="36%" valign="top" style="padding-left:24px">' +
    '<h3 style="color:' + C.fg + ';margin-top:0">Up next</h3>' + list(vids.slice(0, 15), 168, 94) + '</td></tr></table>';
} else if(path === '/watch' || shortId){
  walk(data, 0);
  /* no player data, but the address has the video id: Play works anyway */
  html += playerHtml(vid, '') +
          '<h2 style="color:' + C.fg + '">YouTube sent no details for this video</h2>' +
          '<p style="color:' + C.dim + '">The page arrived without its player data (it may want a sign-in first), but the address has the video id, so the video plays anyway.</p>' +
          list(vids.slice(0, 15), 168, 94);
} else if(path === '/results'){
  walk(data, 0);
  title = query + ' - YouTube';
  html += '<p style="color:' + C.dim + '">' + vids.length + ' videos for "' + esc(query) + '"</p>' + list(vids, 320, 180);
} else {
  var ideas = ['operating system from scratch', 'lofi music', 'how computers work', 'cat videos', 'space documentary'];
  html += '<h2 style="color:' + C.fg + '">Try searching to get started</h2>' +
          '<p style="color:' + C.dim + '">YouTube sends no home feed to visitors who are not signed in. Some ideas:</p><ul>' +
          ideas.map(function(s){ return '<li><a href="/results?search_query=' + encodeURIComponent(s).replace(/%20/g, '+') + '">' + esc(s) + '</a></li>'; }).join('') + '</ul>';
}
d.title = title;
d.body.innerHTML = html;
d.body.style = 'background:' + C.bg + ';color:' + C.fg + ';margin:16px';
if(d.documentElement) d.documentElement.style = 'background:' + C.bg;
function go(){ var q = d.getElementById('q'); var v = q && q.value; if(v) w.location.href = '/results?search_query=' + encodeURIComponent(v).replace(/%20/g, '+'); }
var gb = d.getElementById('go'); if(gb) gb.addEventListener('click', go);
var qb = d.getElementById('q'); if(qb) qb.addEventListener('keydown', function(e){ if(e.key === 'Enter') go(); });
/* ---- the player: Media Source Extensions, as YouTube's own player works ----
 * The stream list comes from the host proxy (/ytinfo: yt-dlp does what YouTube's player
 * script would, turning the id into stream URLs); the streams themselves are YouTube's
 * DASH files, fetched by byte range through the proxy (/ytm) and appended to two
 * SourceBuffers, video and audio. The browser hands them to Reel, the OS's media process,
 * which demuxes and decodes them (H.264, VP9 or AV1; AAC or Opus) and plays them in the
 * <video> box. Seeking uses the index in each file (sidx in MP4, Cues in WebM). */
var PROXY = 'http://10.0.2.2:8888';
var vel = d.getElementById('vid');
if(vel && vid){
  var info = null, ms = null, streams = [], gen = 0, started = false, timer = 0;
  var pref = {codec: 'h264', height: 360};
  var stEl = d.getElementById('vst'), timeEl = d.getElementById('vtime'), pb = d.getElementById('play');
  var PLAY = '\u25B6', PAUSE = '||';
  var lastBar = -1;
  function say(m){ if(stEl) stEl.textContent = m; }
  function clock(t){ t = Math.max(0, Math.floor(t || 0)); var m = Math.floor(t / 60), s2 = t % 60; return m + ':' + (s2 < 10 ? '0' : '') + s2; }
  function family(c){ return /^avc/.test(c) ? 'h264' : /^vp0?9/.test(c) ? 'vp9' : /^av01/.test(c) ? 'av1' : ''; }
  function pick(){
    var vs = info.formats.filter(function(f){ return f.kind === 'video' && !f.hdr && MediaSource.isTypeSupported(f.mime + '; codecs="' + f.codecs + '"'); });
    var want = vs.filter(function(f){ return family(f.codecs) === pref.codec; });
    if(!want.length) want = vs.filter(function(f){ return family(f.codecs) === 'h264'; });
    var fit = want.filter(function(f){ return f.height <= pref.height; });
    if(!fit.length) fit = want.slice().sort(function(a, b){ return a.height - b.height; }).slice(0, 1);
    fit.sort(function(a, b){ return b.height - a.height || a.kbps - b.kbps; });
    var v = fit[0];
    var as = info.formats.filter(function(f){ return f.kind === 'audio' && MediaSource.isTypeSupported(f.mime + '; codecs="' + f.codecs + '"'); });
    /* the audio in the same container as the video, mid quality: AAC 140 or Opus 251 */
    as.sort(function(a, b){
      var ca = a.mime.split('/')[1] === (v ? v.mime.split('/')[1] : 'mp4') ? 0 : 1, cb = b.mime.split('/')[1] === (v ? v.mime.split('/')[1] : 'mp4') ? 0 : 1;
      return ca - cb || Math.abs(a.kbps - 128) - Math.abs(b.kbps - 128);
    });
    return {v: v, a: as[0]};
  }
  /* ---- the index at the start of each file ---- */
  function u8(buf){ return new Uint8Array(buf); }
  function be(b, p, n){ var v = 0; for(var k = 0; k < n; k++) v = v * 256 + b[p + k]; return v; }
  function sidx(b, size){
    for(var p = 0; p + 8 <= b.length;){
      var sz = be(b, p, 4), ty = String.fromCharCode(b[p + 4], b[p + 5], b[p + 6], b[p + 7]);
      if(sz === 1) sz = be(b, p + 8, 8);
      if(sz < 8) break;
      if(ty === 'sidx'){
        if(p + sz > b.length) return null;
        var q = p + 8, ver = b[q]; q += 4 + 4;
        var ts = be(b, q, 4); q += 4;
        var ept = be(b, q, ver ? 8 : 4); q += ver ? 8 : 4;
        var first = be(b, q, ver ? 8 : 4); q += ver ? 8 : 4;
        q += 2; var cnt = be(b, q, 2); q += 2;
        var off = p + sz + first, t = ept / ts, segs = [];
        for(var k = 0; k < cnt; k++){
          var ref = be(b, q, 4) % 2147483648, dur = be(b, q + 4, 4) / ts; q += 12;
          segs.push({t: t, start: off, end: off + ref - 1}); off += ref; t += dur;
        }
        return segs;
      }
      if(ty === 'moof' || ty === 'mdat') break;
      p += sz;
    }
    return null;
  }
  function vint(b, p, keep){
    var c = b[p], n = 1; while(n <= 8 && !(c & (0x80 >> (n - 1)))) n++;
    var v = keep ? c : c & (0xff >> n), all = (c & (0xff >> n)) === (0xff >> n);
    for(var k = 1; k < n; k++){ v = v * 256 + b[p + k]; if(b[p + k] !== 255) all = false; }
    return {v: all && !keep ? -1 : v, n: n};
  }
  function cues(b, size){
    var p = 0, segStart = -1, scale = 1e6, out = null;
    while(p < b.length - 8){
      var id = vint(b, p, 1), sz = vint(b, p + id.n, 0), body = p + id.n + sz.n;
      if(id.v === 0x18538067){ segStart = body; p = body; continue; }            /* Segment: step in */
      if(sz.v < 0 || body + sz.v > b.length) break;
      if(id.v === 0x1549A966){                                                    /* Info: TimecodeScale */
        for(var q = body; q < body + sz.v;){ var ci = vint(b, q, 1), cs = vint(b, q + ci.n, 0); if(ci.v === 0x2AD7B1) scale = be(b, q + ci.n + cs.n, cs.v); q += ci.n + cs.n + cs.v; }
      }
      if(id.v === 0x1C53BB6B){                                                    /* Cues */
        out = [];
        for(var q2 = body; q2 < body + sz.v;){
          var pi = vint(b, q2, 1), ps = vint(b, q2 + pi.n, 0), pb2 = q2 + pi.n + ps.n, t = 0, pos = -1;
          for(var r = pb2; r < pb2 + ps.v;){
            var ei = vint(b, r, 1), es = vint(b, r + ei.n, 0), eb = r + ei.n + es.n;
            if(ei.v === 0xB3) t = be(b, eb, es.v);
            if(ei.v === 0xB7) for(var u = eb; u < eb + es.v;){ var ti = vint(b, u, 1), tz = vint(b, u + ti.n, 0); if(ti.v === 0xF1) pos = be(b, u + ti.n + tz.n, tz.v); u += ti.n + tz.n + tz.v; }
            r = eb + es.v;
          }
          if(pos >= 0) out.push({t: t * scale / 1e9, start: segStart + pos});
          q2 = pb2 + ps.v;
        }
        for(var k = 0; k < out.length; k++) out[k].end = k + 1 < out.length ? out[k + 1].start - 1 : (size ? size - 1 : out[k].start + 4194303);
        return out;
      }
      if(id.v === 0x1F43B675) break;                                              /* the first Cluster: no Cues before it */
      p = body + sz.v;
    }
    return out;
  }
  /* ---- a stream: one SourceBuffer, filled by byte range ---- */
  function Stream(f){
    this.f = f; this.sb = ms.addSourceBuffer(f.mime + '; codecs="' + f.codecs + '"');
    this.pos = 0; this.q = []; this.busy = false; this.eof = false; this.segs = null; this.fails = 0;
    this.chunk = Math.max(262144, Math.round(f.kbps * 125 * 5));        /* about 5 s a request */
    var s = this;
    this.sb.addEventListener('updateend', function(){ feed(s); });
    this.sb.addEventListener('error', function(){ say('The ' + f.kind + ' stream could not be read.'); });
  }
  function feed(s){
    if(s.sb.updating || !s.q.length) return;
    try { s.sb.appendBuffer(s.q.shift()); } catch(e){ say('append: ' + e.message); }
    if(!s.sb.updating) feed(s);
  }
  function ahead(s){
    var b = s.sb.buffered, t = vel.currentTime;
    for(var k = 0; k < b.length; k++) if(t >= b.start(k) - 0.3 && t < b.end(k)) return b.end(k) - t;
    return 0;
  }
  function get(s){
    if(s.busy || s.eof || s.q.length > 1 || ahead(s) > 20) return;
    var a = s.pos, b = a + s.chunk - 1, my = gen;
    if(s.f.size && b >= s.f.size) b = s.f.size - 1;
    s.busy = true;
    fetch(PROXY + '/ytm/' + vid + '/' + s.f.itag + '?r=' + a + '-' + b).then(function(r){
      if(!r.ok) throw new Error('HTTP ' + r.status);
      return r.arrayBuffer();
    }).then(function(buf){
      s.busy = false;
      if(my !== gen) return;                                 /* a seek happened meanwhile */
      s.fails = 0;
      if(s.pos === 0) s.segs = s.f.mime.indexOf('webm') >= 0 ? cues(u8(buf), s.f.size) : sidx(u8(buf), s.f.size);
      s.pos = a + buf.byteLength;
      if(buf.byteLength < b - a + 1 || (s.f.size && s.pos >= s.f.size)) s.eof = true;
      s.q.push(buf);
      feed(s);
      tick();
    }).catch(function(e){
      s.busy = false;
      if(++s.fails > 4){ say('The ' + s.f.kind + ' stream failed: ' + e.message); s.eof = true; }
    });
  }
  function tick(){
    if(!ms || ms.readyState === 'closed') return;
    streams.forEach(get);
    if(ms.readyState === 'open' && streams.length && streams.every(function(s){ return s.eof && !s.q.length && !s.busy && !s.sb.updating; })) ms.endOfStream();
    show();
  }
  function show(){
    var t = vel.currentTime, dur = vel.duration > 0 ? vel.duration : info ? info.duration : 0;
    if(timeEl) timeEl.textContent = clock(t) + ' / ' + clock(dur) + (vel.paused ? '' : '');
    var cell = dur ? Math.min(BAR - 1, Math.floor(t / dur * BAR)) : 0;
    if(cell !== lastBar){
      lastBar = cell;
      var b = vel.buffered;
      for(var k = 0; k < BAR; k++){
        var c = d.getElementById('vb' + k), ct = (k + 0.5) / BAR * dur, buf = false;
        for(var r = 0; r < b.length; r++) if(ct >= b.start(r) && ct < b.end(r)) buf = true;
        if(c) c.style = 'font-size:6px;background:' + (k <= cell ? '#ff0000' : buf ? '#777777' : '#3a3a3a');
      }
    }
  }
  function describe(){
    var v = streams[0] && streams[0].f, a = streams[1] && streams[1].f, q = vel.getVideoPlaybackQuality();
    say((v ? v.codecs.split('.')[0].replace('avc1', 'H.264').replace('vp09', 'VP9').replace('av01', 'AV1') + ' ' + v.height + 'p (itag ' + v.itag + ')' : 'no video') +
        ' + ' + (a ? (a.codecs === 'opus' ? 'Opus' : 'AAC') + ' (itag ' + a.itag + ')' : 'no audio') +
        ' from YouTube, decoded by this OS.');
  }
  function begin(){
    var p = pick();
    if(!p.v && !p.a){ say('YouTube offers no stream this browser can play.'); return; }
    gen++;
    streams = [];
    ms = new MediaSource();
    ms.addEventListener('sourceopen', guard(function(){
      if(ms.readyState !== 'open' || streams.length) return;
      ms.duration = info.duration || NaN;
      if(p.v) streams.push(new Stream(p.v));
      if(p.a) streams.push(new Stream(p.a));
      describe();
      tick();
      vel.play();
    }));
    vel.src = URL.createObjectURL(ms);
    if(!timer) timer = setInterval(tick, 1000);
  }
  function seek(t){
    var dur = vel.duration > 0 ? vel.duration : info.duration;
    t = Math.max(0, Math.min(t, dur - 0.5));
    var inside = streams.every(function(s){ var b = s.sb.buffered; for(var k = 0; k < b.length; k++) if(t >= b.start(k) && t < b.end(k) - 0.5) return true; return false; });
    if(!inside){
      if(!streams.every(function(s){ return s.segs && s.segs.length; })){ say('This stream has no index: it can only seek inside what is loaded.'); return; }
      gen++;
      streams.forEach(function(s){
        var k = 0; while(k + 1 < s.segs.length && s.segs[k + 1].t <= t) k++;
        s.q = []; s.busy = false; s.eof = false;
        try { s.sb.abort(); } catch(e){}
        s.pos = s.segs[k].start;
      });
    }
    vel.currentTime = t;
    lastBar = -1;
    tick();
  }
  function startOrToggle(){
    if(!started){
      started = true;
      say('Asking for the stream list...');
      var tries = 0;
      (function ask(){
        fetch(PROXY + '/ytinfo/' + vid).then(function(r){ if(!r.ok) throw new Error('HTTP ' + r.status); return r.json(); }).then(function(i){
          info = i; begin();
        }).catch(function(e){
          /* the first ask can take the host a while (yt-dlp): ask again */
          if(++tries < 4){ say('Still waiting for the stream list...'); setTimeout(ask, 1500); return; }
          started = false; say('The stream list did not come (is tools/https_proxy.py running?): ' + e.message);
        });
      })();
      return;
    }
    if(vel.paused) vel.play(); else vel.pause();
  }
  /* a handler that fails says so on the page (there is no console to look at) */
  function guard(f){ return function(e){ try { return f(e); } catch(x){ say('Error: ' + (x && x.message || x)); } }; }
  vel.addEventListener('play', function(){ if(pb) pb.textContent = PAUSE; });
  vel.addEventListener('pause', function(){ if(pb) pb.textContent = PLAY; });
  vel.addEventListener('ended', function(){ if(pb) pb.textContent = PLAY; say('The end.'); });
  vel.addEventListener('waiting', function(){ say('Buffering...'); });
  vel.addEventListener('playing', describe);
  vel.addEventListener('error', function(){ say('Playback failed: ' + (vel.error && vel.error.message || 'unknown error')); });
  if(pb) pb.addEventListener('click', guard(startOrToggle));
  vel.addEventListener('click', guard(startOrToggle));
  d.addEventListener('keydown', guard(function(e){
    if(e.key === ' ' || e.key === 'k') startOrToggle();
    else if(info && e.key === 'ArrowLeft') seek(vel.currentTime - 5);
    else if(info && e.key === 'ArrowRight') seek(vel.currentTime + 5);
  }));
  for(var k = 0; k < BAR; k++) (function(k){
    var c = d.getElementById('vb' + k);
    if(c) c.addEventListener('click', guard(function(){ var dur = vel.duration > 0 ? vel.duration : info && info.duration; if(dur) seek(k / BAR * dur); }));
  })(k);
  /* it starts by itself, like the real page */
  setTimeout(guard(startOrToggle), 300);
}
})(this);

