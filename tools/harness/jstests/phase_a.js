var fails = [];
function eq(name, got, want){ if(got !== want) fails.push(name + ": got " + got + " want " + want); }
// numbers
eq("hex", 0xff, 255); eq("dot", .5 + .25, 0.75); eq("exp", 1e-3 * 1000, 1); eq("bin", 0b101, 5); eq("sep", 1_000, 1000);
// strings
eq("uesc", "A\x42", "AB"); eq("surr", "😀".length > 0, true);
// bitwise
eq("or0", 3.7 | 0, 3); eq("and", 12 & 10, 8); eq("xor", 12 ^ 10, 6); eq("shl", 1 << 31, -2147483648);
eq("shr", -16 >> 2, -4); eq("ushr", -1 >>> 28, 15); eq("not", ~5, -6); eq("prec", 1 + 2 << 1, 6); eq("prec2", 5 & 3 == 3, 1);
// compound
var x = 5; x *= 3; x -= 1; x |= 16; x >>= 1; eq("compound", x, 15);
var y = null; y ??= 7; y ||= 9; y &&= y + 1; eq("logical=", y, 8);
// comma
var c = (1, 2, 3); eq("comma", c, 3);
var i, j; for(i = 0, j = 10; i < j; i++, j--); eq("comma-for", i, 5);
// labels
var hits = 0;
outer: for(var a = 0; a < 3; a++){ for(var b = 0; b < 3; b++){ if(b == 1) continue outer; if(a == 2) break outer; hits++; } }
eq("labels", hits, 2);
blk: { hits = 100; break blk; hits = 0; } eq("label-block", hits, 100);
// for-in without var / for-of with destructuring
var o = {p: 1, q: 2}, k, ks = ""; for(k in o) ks += k; eq("forin", ks, "pq");
var sum = 0; for (const [m, n] of [[1, 2], [3, 4]]) sum += m * n; eq("forof-destr", sum, 14);
// regex with / in class
eq("re-class", /[^/]+\//.test("abc/"), true);
// hoisting
function h(){ v = 3; if(false){ var v; } return v; } eq("hoist", h(), 3);
function h2(){ return typeof w; var w = 1; } eq("hoist-undef", h2(), "undefined");
eq("typeof-undeclared", typeof notDeclaredAnywhere, "undefined");
// named function expression recursion
var fact = function f(n){ return n <= 1 ? 1 : n * f(n - 1); }; eq("nfe", fact(5), 120);
// destructuring
var {aa = 1, bb: {cc}, ...rest} = {bb: {cc: 2}, dd: 3, ee: 4}; eq("destr", aa + cc + rest.dd + rest.ee, 10);
var [p1, , p3 = 9, ...pr] = [1, 2, undefined, 4, 5]; eq("arr-destr", p1 + p3 + pr.length, 12);
var s1 = 1, s2 = 2; [s1, s2] = [s2, s1]; eq("swap", s1 * 10 + s2, 21);
var fn = ({u, w = 4}, [z]) => u + w + z; eq("param-destr", fn({u: 1}, [2]), 7);
var def = (a, b = a * 2) => a + b; eq("default", def(3), 9);
// object literal extras
var key = "dyn"; var ob = {[key + "1"]: 1, ...{s: 2}, get g(){ return 3; }}; eq("objlit", ob.dyn1 + ob.s + ob.g, 6);
// ASI-ish: return on its own line
function r(){ return
  5; } eq("asi-return", r(), undefined);
// top-level var on global object
var gtest = 42; eq("global-var", this.gtest, 42);
console.log(fails.length ? fails.join(" | ") : "ALL PASS");
