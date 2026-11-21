/* generators: yield, yield*, next/return/throw, laziness, cleanup */
/* a tiny checker the feature tests share: ok(name, condition) */
var _pass = 0, _fail = 0;
function ok(name, cond){ if(cond) _pass++; else { _fail++; console.log('FAIL: ' + name) } }
function eq(name, a, b){ ok(name + ' (' + a + ' vs ' + b + ')', a === b) }
function done(){ console.log((_fail ? 'FAILED ' : 'passed ') + _pass + ' checks, ' + _fail + ' failures') }

function* counting(){ yield 1; yield 2; return 3 }
var it = counting();
eq('first next', JSON.stringify(it.next()), '{"value":1,"done":false}');
eq('second next', JSON.stringify(it.next()), '{"value":2,"done":false}');
eq('return value', JSON.stringify(it.next()), '{"value":3,"done":true}');
eq('after the end', JSON.stringify(it.next()), '{"done":true}');
eq('spread', [...counting()].join(), '1,2');

var sum = 0;
for (var v of counting()) sum += v;
eq('for-of', sum, 3);

function* echo(){ var a = yield 'first'; var b = yield a * 2; return a + b }
var e = echo();
e.next();
eq('value sent in', e.next(10).value, 20);
eq('return after sends', e.next(5).value, 15);

function* inner(){ yield 'a'; yield 'b'; return 'r' }
function* outer(){ var r = yield* inner(); yield r }
eq('yield*', [...outer()].join(), 'a,b,r');
function* mixed(){ yield* [1, 2]; yield* 'xy'; yield* new Set([9]) }
eq('yield* over any iterable', [...mixed()].join(), '1,2,x,y,9');

var cleaned = false;
function* withFinally(){ try { yield 1; yield 2 } finally { cleaned = true } }
var f = withFinally();
f.next();
eq('return() ends it', JSON.stringify(f.return(9)), '{"value":9,"done":true}');
ok('return() runs finally', cleaned);

cleaned = false;
for (var x of withFinally()) break;
ok('break runs finally', cleaned);

function* catching(){ try { yield 1 } catch (err) { yield 'caught:' + err } }
var c = catching();
c.next();
eq('throw() goes into the body', c.next === undefined ? '' : c.throw('X').value, 'caught:X');

function* endless(){ var i = 0; while (true) yield i++ }
var taken = [];
for (var n of endless()){ if (taken.length === 5) break; taken.push(n) }
eq('for-of is lazy', taken.join(), '0,1,2,3,4');

var obj = { *values(){ yield 7 }, [Symbol.iterator](){ return this.values() } };
eq('generator method', [...obj].join(), '7');

function* recursive(n){ if (n > 0){ yield n; yield* recursive(n - 1) } }
eq('nested delegation', [...recursive(5)].join(), '5,4,3,2,1');

var gen = counting();
ok('generators iterate themselves', gen[Symbol.iterator]() === gen);
eq('toStringTag', Object.prototype.toString.call(gen), '[object Generator]');

/* a generator the collector must keep alive through another generator's stack */
function* holder(){ var g = recursive(3); g.next(); yield 'mid'; yield g.next().value }
var h = holder();
h.next();
for (var i = 0; i < 2000; i++){ var junk = []; for (var j = 0; j < 40; j++) junk.push({ j: j }) }
eq('suspended stack survives a collection', h.next().value, 2);

done();
