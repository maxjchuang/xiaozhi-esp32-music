const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const source = fs.readFileSync(`${__dirname}/character-phase-four.html`, 'utf8').match(/<script>([\s\S]*?)<\/script>/)[1];
let now = 0, depth = 0;
const noop = () => {};
const context = new Proxy({
  save() { depth++; },
  restore() { assert(depth > 0); depth--; },
}, {
  get: (o, k) => o[k] ?? ((...args) => assert(args.filter(v => typeof v === 'number').every(Number.isFinite))),
  set: (o, k, v) => (o[k] = v, true),
});
const nodes = new Map();
const makeNode = () => ({
  value: 'quiet', checked: false, dataset: {}, children: [],
  getContext: () => context, append(x) { this.children.push(x); },
  replaceChildren: noop, querySelectorAll() { return this.children; },
  setAttribute: noop, classList: { toggle: noop },
});
const math = Object.create(Math);
math.random = () => .5;
const sandbox = {
  Math: math, performance: { now: () => now }, requestAnimationFrame: noop,
  document: {
    hidden: false,
    querySelector(s) { if (!nodes.has(s)) nodes.set(s, makeNode()); return nodes.get(s); },
    querySelectorAll: () => [], createElement: makeNode, addEventListener: noop,
  },
};
vm.createContext(sandbox);
const run = s => vm.runInContext(s, sandbox);
run(source);
const tick = n => {
  for (let i = 0; i < n; i++) {
    now += 16; run(`frame(${now})`);
    assert.equal(depth, 0);
    assert(run('Object.values(pose).every(Number.isFinite)'));
  }
};
assert.equal(nodes.get('#acts').children.length, 11);
for (const key of run('Object.keys(acts)')) {
  run(`setState('${key}')`); tick(500);
  assert.equal(run('state'), key === 'rub' ? 'sleepy' : 'idle');
  run(`setState('${key}')`); tick(100);
  nodes.get('#interrupt').onclick();
  assert.equal(run('state'), 'listen');
}
for (const instrument of ['shaker', 'drum', 'keys', 'guitar']) {
  run(`musicPreview.instrument='${instrument}';startMusic()`); tick(600);
  assert.equal(run('state'), instrument);
  const start = run('musicPreview.started');
  nodes.get('#music-view').onchange({ target: { value: 'cover' } }); tick(5);
  assert.equal(run('state'), 'idle');
  assert(run('idlePlay.music'));
  nodes.get('#music-view').onchange({ target: { value: 'cat' } });
  assert.equal(run('state'), instrument);
  assert.equal(run('musicPreview.started'), start);
  nodes.get('#face').onpointerdown({});
  assert.equal(run('state'), 'listen');
  assert(!run('musicPreview.playing'));
}
run('startMusic()'); now += 60001; run(`frame(${now})`);
assert.equal(run('state'), 'idle');
assert(!run('musicPreview.playing'));
run("setState('idle');idlePlay.lastEnd=-Infinity;scheduleIdle(performance.now())");
assert.equal(run('idlePlay.nextAt-performance.now()'), 135000);
now = run('idlePlay.nextAt'); run(`frame(${now})`);
assert(run('idlePlay.automatic'));
const previous = run('state'); tick(500);
assert(run('idlePlay.nextAt-idlePlay.lastEnd>=120000'));
now = run('idlePlay.nextAt'); run(`frame(${now})`);
assert.notEqual(run('state'), previous);
nodes.get('#face').onpointerdown({});
assert.equal(run('state'), 'listen');
for (let i = 0; i < 105; i++) {
  const q = run(`guitarStrum(${i / 100})`), angle = q.angle - .49;
  assert(Math.sin(angle) > 0 && -Math.cos(angle) < 0);
}
console.log('PASS: 11 acts, exits, interruption, 4 instruments, scene switching, natural finish, idle cooldown/nonrepeat, paw direction and finite drawing. Mock canvas only.');
