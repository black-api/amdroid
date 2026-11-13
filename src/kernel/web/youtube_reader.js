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
var title = 'YouTube';
if(path === '/watch' && player && player.videoDetails){
  var vd = player.videoDetails;
  title = vd.title + ' - YouTube';
  var date = ''; (function find(o, depth){ if(!o || typeof o !== 'object' || depth > 30 || date) return;
    if(o.videoPrimaryInfoRenderer){ date = txt(o.videoPrimaryInfoRenderer.dateText); return; } for(var k in o) find(o[k], depth + 1); })(data, 0);
  walk(data, 0);
  html += '<div>' +
    '<div id="player"><img src="' + thumb(vd.videoId, 1) + '" width="640" height="480" alt="' + esc(vd.title) + '"><br>' +
    '<input type="button" id="play" value="Play"></div>' +
    '<h2 style="color:' + C.fg + '">' + esc(vd.title) + '</h2>' +
    '<p style="color:' + C.fg + '"><b>' + esc(vd.author) + '</b></p>' +
    '<p style="color:' + C.dim + '">' + num(vd.viewCount) + ' views' + (date ? ' - ' + esc(date) : '') + '</p>' +
    '<p style="color:' + C.fg + ';background:' + C.box + ';padding:8px">' + esc(vd.shortDescription || '').replace(/\n/g, '<br>') + '</p>' +
    '<h3 style="color:' + C.fg + '">Up next</h3>' + list(vids.slice(0, 15), 168, 94) + '</div>';
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
/* Play: what YouTube actually sent, and why the OS cannot play it */
var pb = d.getElementById('play');
if(pb) pb.addEventListener('click', function(){
  var sd = (player && player.streamingData) || {}, fs = (sd.formats || []).concat(sd.adaptiveFormats || []);
  var codecs = {}, withUrl = 0;
  fs.forEach(function(f){ if(f.url || f.signatureCipher || f.cipher) withUrl++;
    var mt = /^(\w+)\/(\w+);\s*codecs="([a-z0-9]+)/i.exec(f.mimeType || ''); if(mt){ var n = {avc1: 'H.264', vp9: 'VP9', av01: 'AV1', mp4a: 'AAC', opus: 'Opus'}[mt[3]] || mt[3]; codecs[n] = 1; } });
  var host = ''; var hm = /^https?:\/\/([^\/]+)/.exec(sd.serverAbrStreamingUrl || ''); if(hm) host = hm[1];
  var p = d.getElementById('player');
  p.innerHTML = '<div style="background:#000000;color:#ffffff;padding:24px;width:592px">' +
    '<h2 style="color:#ff5555">This OS can\'t play YouTube (yet)</h2>' +
    '<p>YouTube sent <b>' + fs.length + '</b> versions of this video (' + Object.keys(codecs).join(', ') + ').</p>' +
    '<p><b>' + withUrl + ' of ' + fs.length + '</b> come with an address to download them from.</p>' +
    (host ? '<p>Instead, the video only streams through YouTube\'s own protocol (SABR) at <b>' + esc(host) + '</b>, ' +
            'which wants a proof-of-origin token from BotGuard, YouTube\'s anti-bot check.</p>' :
            '<p>The video is not offered as a plain file.</p>') +
    '<p style="color:#aaaaaa">Everything on this page was read from YouTube\'s own page data by the am-droid browser.</p></div>';
});
})(this);

