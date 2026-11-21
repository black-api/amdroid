/* Proxy and Reflect, tagged templates, iteration and the smaller ES2015+ fixes */
/* a tiny checker the feature tests share: ok(name, condition) */
var _pass = 0, _fail = 0;
function ok(name, cond){ if(cond) _pass++; else { _fail++; console.log('FAIL: ' + name) } }
function eq(name, a, b){ ok(name + ' (' + a + ' vs ' + b + ')', a === b) }
function done(){ console.log((_fail ? 'FAILED ' : 'passed ') + _pass + ' checks, ' + _fail + ' failures') }

/* --- Proxy --- */
var p = new Proxy({}, { get: function(t, k){ return k in t ? t[k] : 'missing:' + k } });
p.here = 1;
eq('proxy get trap', p.nothere, 'missing:nothere');
eq('proxy get falls through', p.here, 1);

var target = {};
var doubling = new Proxy(target, { set: function(t, k, v){ t[k] = v * 2; return true } });
doubling.n = 5;
eq('proxy set trap', target.n, 10);

ok('proxy has trap', 'anything' in new Proxy({}, { has: function(){ return true } }));
eq('proxy ownKeys trap', Object.keys(new Proxy({ a: 1 }, { ownKeys: function(){ return ['a', 'z'] } })).join(), 'a,z');

var delTarget = { a: 1 };
var deleter = new Proxy(delTarget, { deleteProperty: function(t, k){ delete t[k]; return true } });
delete deleter.a;
eq('proxy deleteProperty trap', JSON.stringify(delTarget), '{}');

function add(a, b){ return a + b }
var tenfold = new Proxy(add, { apply: function(t, self, args){ return t.apply(self, args) * 10 } });
eq('proxy apply trap', tenfold(1, 2), 30);
eq('a function proxy is a function', typeof tenfold, 'function');

var plain = new Proxy({ m: function(){ return 'm' } }, {});
eq('proxy without traps', plain.m(), 'm');
var forin = [];
for (var k in new Proxy({ a: 1, b: 2 }, {})) forin.push(k);
eq('for-in over a proxy', forin.join(), 'a,b');

/* --- Reflect --- */
eq('Reflect.get', Reflect.get({ a: 1 }, 'a'), 1);
ok('Reflect.has', Reflect.has({ a: 1 }, 'a'));
eq('Reflect.ownKeys', Reflect.ownKeys({ a: 1, b: 2 }).join(), 'a,b');

/* --- tagged templates --- */
function tag(strings){ return strings.length + '|' + strings.join('~') + '|' + [].slice.call(arguments, 1).join(',') }
eq('tagged template', tag`a${1}b${2}c`, '3|a~b~c|1,2');
eq('tagged template, no holes', tag`plain`, '1|plain|');
function rawOf(s){ return s.raw[0] }
eq('raw strings keep escapes', rawOf`a\nb`.length, 4);
eq('String.raw', String.raw`a\nb${1}`, 'a\\nb1');
var holder = { m: function(s, v){ return 'm:' + s[0] + v } };
eq('tagged method', holder.m`x${5}`, 'm:x5');

/* --- iteration --- */
var fromSet = [];
var [first, second] = new Set([1, 2]);
eq('destructuring an iterable', first + ',' + second, '1,2');
eq('destructuring a string', (function(){ var [a, b] = 'hi'; return a + b })(), 'hi');
eq('spread a Map', [...new Map([['a', 1]])].length, 1);

/* --- let is bound per turn of the loop --- */
var fns = [];
for (let i = 0; i < 3; i++) fns.push(function(){ return i });
eq('let per iteration', fns.map(function(f){ return f() }).join(), '0,1,2');
var vfns = [];
for (var j = 0; j < 3; j++) vfns.push(function(){ return j });
eq('var is still shared', vfns[0](), 3);

/* --- odds and ends that used to be wrong --- */
eq('parseInt radix', parseInt('ff', 16), 255);
eq('parseInt hex prefix', parseInt('0x1f'), 31);
eq('parseInt binary radix', parseInt('101', 2), 5);
ok('parseInt rejects a bad radix', isNaN(parseInt('1', 40)));
ok('includes finds NaN', [NaN].includes(NaN));
ok('indexOf still does not', [NaN].indexOf(NaN) === -1);
function twoArgs(a, b){ return a }
eq('function length', twoArgs.length, 2);
eq('function name', twoArgs.name, 'twoArgs');
function withDefault(a, b = 1, c){ return a }
eq('length stops at a default', withDefault.length, 1);
eq('globalThis', typeof globalThis, 'object');
ok('globalThis is the global', globalThis.Math === Math);

/* --- DataView over a shared buffer --- */
var buf = new ArrayBuffer(8);
var bytes = new Uint8Array(buf);
bytes[0] = 7;
eq('byte views share the buffer', new Uint8Array(buf)[0], 7);
var view = new DataView(buf);
eq('DataView sees the same bytes', view.getUint8(0), 7);
view.setUint16(2, 258);
eq('big endian by default', view.getUint8(2) * 256 + view.getUint8(3), 258);
view.setUint16(2, 258, true);
eq('little endian when asked', view.getUint8(2), 2);
view.setInt32(4, -5);
eq('signed 32-bit', view.getInt32(4), -5);
view.setFloat64(0, -2.25);
eq('float64', view.getFloat64(0), -2.25);
view.setFloat32(0, 1.5);
eq('float32', view.getFloat32(0), 1.5);
eq('byteLength', view.byteLength, 8);
eq('an offset view', new DataView(buf, 4, 4).byteLength, 4);
var threw = false;
try { view.getUint8(100) } catch (e) { threw = e instanceof RangeError }
ok('out of range throws RangeError', threw);

done();
