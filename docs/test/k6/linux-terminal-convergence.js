import http from 'k6/http';
import { check } from 'k6';
import { Counter, Rate, Trend } from 'k6/metrics';

const fatalTerminalLatency = new Trend('fatal_terminal_latency', true);
const rstCount = new Counter('rst_count');
const finReverseFlowChecks = new Counter('fin_reverse_flow_checks');
const rstExpected = new Counter('rst_expected');
const resetObserved = new Rate('reset_observed');

const scenario = __ENV.SCENARIO || 'baseline';
const target = __ENV.TARGET_URL || 'https://127.0.0.1:18080';

const scenarios = {
  baseline: { executor: 'constant-vus', vus: 100, duration: '5m' },
  peak: { executor: 'constant-arrival-rate', rate: 50, timeUnit: '1s', duration: '10m',
    preAllocatedVUs: 500, maxVUs: 500 },
  spike: { executor: 'ramping-vus', startVUs: 100,
    stages: [{ duration: '30s', target: 1000 }, { duration: '2m', target: 1000 }] },
  stress: { executor: 'ramping-vus', startVUs: 250,
    stages: [250, 500, 750, 1000].map((targetVUs) => ({ duration: '2m', target: targetVUs })) },
  soak: { executor: 'constant-arrival-rate', rate: 100, timeUnit: '1s', duration: '30m',
    preAllocatedVUs: 100, maxVUs: 100 },
};

if (!Object.prototype.hasOwnProperty.call(scenarios, scenario)) {
  throw new Error(`unsupported SCENARIO=${scenario}`);
}

export const options = {
  insecureSkipTLSVerify: true,
  scenarios: { [scenario]: scenarios[scenario] },
  thresholds: {
    'checks{workload_normal:true}': ['rate>0.999'],
    'http_req_failed{workload_normal:true}': ['rate<0.001'],
    dropped_iterations: ['count==0'],
    'http_req_duration{workload_normal:true}': ['p(95)<500', 'p(99)<1000'],
    fatal_terminal_latency: ['p(95)<1000', 'p(99)<5000'],
    reset_observed: ['rate>0.999'],
  },
};

function payloadSize() {
  const pick = (__VU * 1103515245 + __ITER * 12345) % 10;
  if (pick < 5) return 1024;
  if (pick < 9) return 64 * 1024;
  return 1024 * 1024;
}

export default function () {
  const rst = ((__VU + __ITER) % 5) === 0; // deterministic 20% RST / 80% FIN
  const body = 'x'.repeat(payloadSize());
  const response = http.post(`${target}/${rst ? 'rst' : 'fin'}`, body, {
    headers: { 'Content-Type': 'application/octet-stream' },
    tags: rst ? { expected_reset: 'true' } : { workload_normal: 'true' },
    timeout: '10s',
  });
  const ok = rst ? false : check(response, {
    'terminal request accepted': (r) => r.status === 200,
    'FIN preserves reverse flow': (r) => r.body === 'reverse-flow-ok',
  }, { workload_normal: 'true' });
  if (rst) {
    rstExpected.add(1);
    resetObserved.add(response.status === 0);
    rstCount.add(1);
    // A reset has no tunnel id visible to k6. Do not attribute a process-global
    // terminal counter to this iteration; the runner rejects the missing
    // one-to-one latency samples until a tunnel-correlated Admin/trace API exists.
  } else if (ok) {
    finReverseFlowChecks.add(1);
  }
}

export function handleSummary(data) {
  const compact = {
    scenario,
    generated_at: new Date().toISOString(),
    passed: Object.values(data.metrics)
      .filter((metric) => metric.thresholds)
      .every((metric) => Object.values(metric.thresholds).every((threshold) => threshold.ok)),
    terminal_convergence: {
      fatal_terminal_latency: data.metrics.fatal_terminal_latency || null,
      rst_count: data.metrics.rst_count || null,
      rst_expected: data.metrics.rst_expected || null,
      reset_observed: data.metrics.reset_observed || null,
      fin_reverse_flow_checks: data.metrics.fin_reverse_flow_checks || null,
    },
    metrics: data.metrics,
  };
  return { 'summary.json': JSON.stringify(compact, null, 2) };
}
