/* typed arrays over shared bytes: views alias, DataView, subarray, endianness */
/* a tiny checker the feature tests share: ok(name, condition) */
var _pass = 0, _fail = 0;
function ok(name, cond){ if(cond) _pass++; else { _fail++; console.log('FAIL: ' + name) } }
function eq(name, a, b){ ok(name + ' (' + a + ' vs ' + b + ')', a === b) }
function done(){ console.log((_fail ? 'FAILED ' : 'passed ') + _pass + ' checks, ' + _fail + ' failures') }

var buf = new ArrayBuffer(8);
var u8 = new Uint8Array(buf), u32 = new Uint32Array(buf), dv = new DataView(buf);
u32[0] = 0x11223344;
eq('u32 write shows in u8 (little endian)', u8[0], 0x44);
eq('u8[3]', u8[3], 0x11);
u8[4] = 0xff; u8[5] = 0x01;
eq('u8 write shows in u32', u32[1], 0x1ff);
eq('DataView reads the same bytes', dv.getUint16(4, true), 0x1ff);
dv.setUint32(0, 0xAABBCCDD);              /* big endian */
eq('DataView write shows in u8', u8[0], 0xAA);
eq('... and in u32', u32[0], 0xDDCCBBAA);
var f = new Float32Array(1); f[0] = 1.5;
var fb = new Uint8Array(f.buffer);
eq('float bytes', Array.from(fb).join(','), '0,0,192,63');
fb[3] = 64;
eq('byte write changes float (0x40C00000)', f[0], 6);
var a = new Uint8Array([1, 2, 3, 4, 5, 6]);
var sub = a.subarray(2, 5);
sub[0] = 99;
eq('subarray aliases', a[2], 99);
a[3] = 77;
eq('parent write shows in subarray', sub[1], 77);
eq('subarray byteOffset', sub.byteOffset, 2);
eq('subarray buffer is the same', sub.buffer === a.buffer, true);
var i16 = new Int16Array(a.buffer, 2, 2);
eq('offset view', i16[0], 99 | (77 << 8));
a.fill(0);
eq('fill through method writes back', i16[0], 0);
i16.set([-1, 2]);
eq('set() shows in byte view', a[2] + ',' + a[3] + ',' + a[4], '255,255,2');
eq('isView typed', ArrayBuffer.isView(i16), true);
eq('isView DataView', ArrayBuffer.isView(dv), true);
eq('isView plain', ArrayBuffer.isView({}), false);
var copy = buf.slice(0, 4);
eq('ArrayBuffer.slice copies', new Uint8Array(copy)[0], 0xAA);
u8[0] = 1;
eq('slice is a copy', new Uint8Array(copy)[0], 0xAA);
var big = new Uint8Array(1000);
for(var i = 0; i < 1000; i++) big[i] = i;
eq('owner view stays fast and right', big[999], 999 & 255);
var s = 0; big.forEach(function(x){ s += x; });
eq('forEach sum', s, 127500 - 128 * 0 - (function(){ var t = 0; for(var i = 0; i < 1000; i++) t += i & 255; return 127500 - t; })());
var [p, q] = new Uint16Array(new Uint8Array([1, 0, 2, 0]).buffer);
eq('destructuring a view', p + q, 3);
eq('spread a view', [...new Int8Array([255, 1])].join(','), '-1,1');
var td = new TextDecoder().decode(new Uint8Array([104, 105]));
eq('TextDecoder on view', td, 'hi');
done();
