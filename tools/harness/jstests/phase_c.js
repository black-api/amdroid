var fails = [];
function eq(name, got, want){ if(got !== want) fails.push(name + ": got " + got + " want " + want); }
// Function.prototype
function add(a, b){ return this.base + a + b; }
eq("call", add.call({base: 1}, 2, 3), 6);
eq("apply", add.apply({base: 1}, [2, 3]), 6);
var b = add.bind({base: 10}, 1); eq("bind", b(2), 13);
eq("apply-max", Math.max.apply(null, [3, 9, 2]), 9);
eq("fn-ctor", new Function("a", "b", "return a * b")(6, 7), 42);
eq("global-this", Function("return this")() === this, true);
// prototypes
function P(x){ this.x = x; } P.prototype.get = function(){ return this.x; };
eq("proto", new P(5).get(), 5); eq("instanceof", new P(1) instanceof P, true);
function Q(){ P.call(this, 2); } Q.prototype = Object.create(P.prototype); Q.prototype.constructor = Q;
eq("inherit", new Q().get(), 2); eq("inherit-of", new Q() instanceof P, true);
// Object
var o = {}; Object.defineProperty(o, "hid", {value: 1, enumerable: false}); o.vis = 2;
eq("defprop-keys", Object.keys(o).join(), "vis"); eq("defprop-val", o.hid, 1);
var acc = {}; var store = 0; Object.defineProperty(acc, "v", {get: function(){ return store * 2; }, set: function(x){ store = x; }});
acc.v = 21; eq("accessor", acc.v, 42);
eq("hasOwn", ({a: 1}).hasOwnProperty("a") && !({}).hasOwnProperty("toString"), true);
eq("toString-tag", Object.prototype.toString.call([]), "[object Array]");
eq("getProto", Object.getPrototypeOf([]) === Array.prototype, true);
class A { get v(){ return this._v; } set v(x){ this._v = x + 1; } } var aa = new A(); aa.v = 1; eq("inherited-setter", aa.v, 2);
// Errors
try { null.x; } catch(e){ eq("typeerror", e instanceof TypeError, true); eq("err-msg", typeof e.message, "string"); }
try { throw new RangeError("r"); } catch(e){ eq("rangeerror", e.name + ":" + e.message, "RangeError:r"); eq("err-is-error", e instanceof Error, true); }
eq("err-str", String(new Error("boom")), "Error: boom");
// Symbol, iterators
var s1 = Symbol("x"); eq("symbol-type", typeof s1, "symbol"); eq("symbol-unique", s1 !== Symbol("x"), true);
var it = {}; it[Symbol.iterator] = function(){ var i = 0; return {next: function(){ return i < 3 ? {value: i++, done: false} : {done: true}; }}; };
var got = []; for(var v of it) got.push(v); eq("custom-iter", got.join(), "0,1,2");
eq("spread-iter", [...it].length, 3);
// Array
var arr = [1, 2, 3, 4, 5]; var rm = arr.splice(1, 2, "a", "b", "c"); eq("splice", arr.join() + "|" + rm.join(), "1,a,b,c,4,5|2,3");
eq("new-array", new Array(3).length, 3); eq("array-of-args", Array(1, 2).join("-"), "1-2");
var big = []; for(var i = 0; i < 2000; i++) big.push("abcdef"); eq("join-big", big.join("").length, 12000);
eq("from-set", Array.from(new Set([1, 1, 2])).join(), "1,2"); eq("from-len", Array.from({length: 3}, (_, i) => i * 2).join(), "0,2,4");
// String / Number / Math
eq("substr", "abcdef".substr(2, 3), "cde"); eq("lastIndexOf", "abcabc".lastIndexOf("b"), 4);
eq("fromCharCode", String.fromCharCode(72, 105), "Hi"); eq("num-parse", Number(" 0x10 "), 16); eq("num-ws", +" 12 ", 12);
eq("isInteger", Number.isInteger(5) && !Number.isInteger(5.5), true); eq("trunc", Math.trunc(-4.7), -4); eq("sign", Math.sign(-3), -1);
eq("imul", Math.imul(0xffffffff, 5), -5); eq("clz32", Math.clz32(1), 31); eq("log2", Math.round(Math.log2(1024)), 10);
eq("uri", encodeURIComponent("a b&c=d/é"), "a%20b%26c%3Dd%2F%C3%A9"); eq("uri-dec", decodeURIComponent("a%20b%C3%A9"), "a bé");
eq("btoa", btoa("hello"), "aGVsbG8="); eq("atob", atob("aGVsbG8="), "hello");
// Date
var d = new Date(Date.UTC(2020, 1, 29, 12, 30)); eq("date-iso", d.toISOString(), "2020-02-29T12:30:00.000Z");
eq("date-parts", d.getUTCFullYear() + "-" + d.getUTCMonth() + "-" + d.getUTCDate() + "-" + d.getUTCDay(), "2020-1-29-6");
eq("date-parse", Date.parse("2020-02-29T12:30:00.000Z"), d.getTime()); eq("date-arith", new Date(0) - 0, 0);
eq("date-now", Date.now() > 1.7e12, true);
// Map / Set / WeakMap
var m = new Map([["a", 1]]); var ko = {}; m.set(ko, 2).set(NaN, 3);
eq("map", m.get("a") + m.get(ko) + m.get(NaN) + m.size, 9); m.delete("a"); eq("map-del", m.has("a") + ":" + m.size, "false:2");
var ks = []; m.forEach(function(v, k){ ks.push(v); }); eq("map-order", ks.join(), "2,3");
eq("map-entries", [...new Map([[1, 2]])][0].join(), "1,2");
var st = new Set("hello"); eq("set-str", [...st].join(""), "helo");
var wm = new WeakMap(); wm.set(ko, "w"); eq("weakmap", wm.get(ko), "w");
// typed arrays
var u8 = new Uint8Array([1, 256, -1]); eq("u8", u8.join(), "1,0,255"); u8[0] = 300; eq("u8-set", u8[0], 44);
var i32 = new Int32Array(2); i32[0] = 2147483648; eq("i32", i32[0], -2147483648);
eq("textenc", new TextEncoder().encode("hé").length, 3);
// Promise ordering
var log = [];
Promise.resolve(1).then(function(v){ log.push("a" + v); return v + 1; }).then(function(v){ log.push("b" + v); });
new Promise(function(res){ log.push("sync"); res(5); }).then(function(v){ log.push("c" + v); });
Promise.all([1, Promise.resolve(2)]).then(function(v){ log.push("all" + v.join("")); });
Promise.reject(new Error("x")).catch(function(e){ log.push("caught"); }).finally(function(){ log.push("fin"); });
log.push("end");
setTimeout(function(){
  eq("promise-order", log.join(" "), "sync end a1 c5 caught b2 all12 fin");
  console.log(fails.length ? fails.join(" | ") : "ALL PASS");
}, 0);
console.log("pending");
