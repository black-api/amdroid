/* async functions, await, async generators and for await */
/* a tiny checker the feature tests share: ok(name, condition) */
var _pass = 0, _fail = 0;
function ok(name, cond){ if(cond) _pass++; else { _fail++; console.log('FAIL: ' + name) } }
function eq(name, a, b){ ok(name + ' (' + a + ' vs ' + b + ')', a === b) }
function done(){ console.log((_fail ? 'FAILED ' : 'passed ') + _pass + ' checks, ' + _fail + ' failures') }

var waiting = [];
function later(p){ waiting.push(p); return p }

var order = [];

async function one(){ return 1 }
async function addOne(){ return (await one()) + 1 }
async function boom(){ throw new Error('boom') }
async function catches(){ try { await boom() } catch (e) { return 'caught ' + e.message } }

ok('an async call returns a promise', one() instanceof Promise);

later(one().then(function(v){ eq('plain return', v, 1) }));
later(addOne().then(function(v){ eq('await', v, 2) }));
later(boom().catch(function(e){ eq('rejection', e.message, 'boom') }));
later(catches().then(function(v){ eq('try/catch around await', v, 'caught boom') }));

var expr = async function(){ await one(); return 'expr' };
later(expr().then(function(v){ eq('async function expression', v, 'expr') }));
var fat = async (x) => (await one()) + x;
later(fat(10).then(function(v){ eq('async arrow', v, 11) }));
var short = async x => x * 2;
later(short(21).then(function(v){ eq('async arrow without braces', v, 42) }));

var methods = { async m(){ return 'method' } };
later(methods.m().then(function(v){ eq('async method', v, 'method') }));
class K { async m(){ return 'class' } static async s(){ return 'static' } }
later(new K().m().then(function(v){ eq('async class method', v, 'class') }));
later(K.s().then(function(v){ eq('static async method', v, 'static') }));

async function inLoop(){ var s = 0; for (var i = 0; i < 3; i++) s += await one(); return s }
later(inLoop().then(function(v){ eq('await in a loop', v, 3) }));

async function combined(){ return (await Promise.all([one(), Promise.resolve(2), 3])).join() }
later(combined().then(function(v){ eq('await Promise.all', v, '1,2,3') }));

async function rejected(){ try { await Promise.reject('r') } catch (e) { return 'got ' + e } }
later(rejected().then(function(v){ eq('await a rejected promise', v, 'got r') }));

async function overMixed(){
  var seen = [];
  for await (var v of [one(), 2, Promise.resolve(3)]) seen.push(v);
  return seen.join();
}
later(overMixed().then(function(v){ eq('for await over promises', v, '1,2,3') }));

async function* stream(){ yield 1; await one(); yield 2; return 'end' }
ok('async generator next returns a promise', stream().next() instanceof Promise);

async function drain(){
  var seen = [];
  for await (var v of stream()) seen.push(v);
  return seen.join();
}
later(drain().then(function(v){ eq('for await over an async generator', v, '1,2') }));

async function nestedAwait(){ return await (async function(){ return await one() })() }
later(nestedAwait().then(function(v){ eq('await through nested calls', v, 1) }));

/* everything after an await runs once the synchronous part is finished */
async function ordered(){ order.push('before'); await one(); order.push('after') }
later(ordered().then(function(){ eq('await defers the rest', order.join(), 'before,sync,after') }));
order.push('sync');

Promise.all(waiting).then(function(){ done() }, function(e){ console.log('FAIL: a test promise rejected: ' + e); done() });
