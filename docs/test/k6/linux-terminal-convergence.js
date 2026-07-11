import http from 'k6/http';
import { check, sleep } from 'k6';
import { Counter, Trend } from 'k6/metrics';

const fatalTerminalLatency = new Trend('fatal_terminal_latency', true);
const rstCount = new Counter('rst_count');
const finReverseFlowChecks = new Counter('fin_reverse_flow_checks');

const scenario = __ENV.SCENARIO || 'baseline';
const target = __ENV.TARGET_URL || 'https://127.0.0.1:18080';
const adminUrl = __ENV.CLIENT_ADMIN_URL || '';
const adminToken = __ENV.ADMIN_TOKEN || '';

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
    checks: ['rate>0.999'],
    http_req_failed: ['rate<0.001'],
    dropped_iterations: ['count==0'],
    http_req_duration: ['p(95)<500', 'p(99)<1000'],
    fatal_terminal_latency: ['p(95)<1000', 'p(99)<5000'],
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
  let terminalBefore = null;
  if (rst && adminUrl && adminToken) {
    const snapshot = http.get(`${adminUrl}/api/v1/relay/metrics`, {
      headers: { Authorization: `Bearer ${adminToken}` }, tags: { control_plane: 'true' },
    });
    if (snapshot.status === 200) terminalBefore = snapshot.json('terminal_observed');
  }
  const started = Date.now();
  const response = http.post(`${target}/${rst ? 'rst' : 'fin'}`, body, {
    headers: { 'Content-Type': 'application/octet-stream' },
    timeout: '10s',
  });
  const ok = check(response, {
    'terminal request accepted': (r) => r.status === 200,
    'FIN preserves reverse flow': (r) => rst || r.body === 'reverse-flow-ok',
  });
  if (rst) {
    rstCount.add(1);
    let observed = false;
    for (let attempt = 0; attempt < 50 && terminalBefore !== null; attempt += 1) {
      const snapshot = http.get(`${adminUrl}/api/v1/relay/metrics`, {
        headers: { Authorization: `Bearer ${adminToken}` }, tags: { control_plane: 'true' },
      });
      if (snapshot.status === 200 && snapshot.json('terminal_observed') > terminalBefore) {
        observed = true; break;
      }
      sleep(0.1);
    }
    if (observed) fatalTerminalLatency.add(Date.now() - started);
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
      fin_reverse_flow_checks: data.metrics.fin_reverse_flow_checks || null,
    },
    metrics: data.metrics,
  };
  return { 'summary.json': JSON.stringify(compact, null, 2) };
}
