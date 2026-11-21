/* classes: static members, fields, private names, computed keys, accessors, new.target */
/* a tiny checker the feature tests share: ok(name, condition) */
var _pass = 0, _fail = 0;
function ok(name, cond){ if(cond) _pass++; else { _fail++; console.log('FAIL: ' + name) } }
function eq(name, a, b){ ok(name + ' (' + a + ' vs ' + b + ')', a === b) }
function done(){ console.log((_fail ? 'FAILED ' : 'passed ') + _pass + ' checks, ' + _fail + ' failures') }

class Base {
  static made = 0;
  static tag;
  static { Base.tag = 'v1' }
  #secret = 7;
  size = 0;
  label = 'b' + this.#secret;
  constructor(name){ this.name = name; Base.made++ }
  static of(n){ return new Base(n) }
  get secret(){ return this.#secret }
  set secret(v){ this.#secret = v }
  reveal(){ return this.#secret }
  ['computed' + 'Key'](){ return 'ck' }
  static ['s' + 'Key'](){ return 'sk' }
  *[Symbol.iterator](){ yield this.name }
}

class Derived extends Base {
  kind = 'derived';
  constructor(n){ super(n); this.after = true }
  reveal(){ return super.reveal() + 1 }
  static child(){ return 'c' }
}

var b = new Base('x');
eq('field', b.size, 0);
eq('field sees private', b.label, 'b7');
eq('private method', b.reveal(), 7);
eq('getter', b.secret, 7);
b.secret = 9;
eq('setter', b.reveal(), 9);
eq('computed method', b.computedKey(), 'ck');
eq('computed static', Base.sKey(), 'sk');
eq('static field', Base.made, 1);
eq('static block', Base.tag, 'v1');
eq('static method', Base.of('y').name, 'y');
eq('class name', Base.name, 'Base');
eq('generator member', [...b].join(), 'x');
ok('private is hidden', Object.keys(b).indexOf('#secret') < 0);
ok('methods are not enumerable', Object.keys(b).join() === 'size,label,name');

var d = new Derived('z');
eq('derived field', d.kind, 'derived');
eq('base field through super', d.label, 'b7');
eq('super call', d.reveal(), 8);
eq('ctor after super', d.after, true);
eq('static inherited', Derived.of('q').name, 'q');
eq('own static', Derived.child(), 'c');
ok('instanceof base', d instanceof Base);

var Anon = class { m(){ return 'anon' } };
eq('class expression', new Anon().m(), 'anon');
var Named = class Inner { m(){ return Inner.name } };
eq('named class expression', new Named().m(), 'Inner');

function F(){ this.nt = new.target === F }
eq('new.target in new', new F().nt, true);
function G(){ return new.target === undefined }
eq('new.target in call', G(), true);

class HasInstance { static [Symbol.hasInstance](){ return true } }
ok('Symbol.hasInstance', ({}) instanceof HasInstance);

class Prim { [Symbol.toPrimitive](hint){ return hint === 'number' ? 42 : 'str' } }
eq('Symbol.toPrimitive number', +new Prim(), 42);
eq('Symbol.toPrimitive string', `${new Prim()}`, 'str');

done();
