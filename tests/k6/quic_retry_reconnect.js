import http from 'k6/http';
import { check } from 'k6';

const profiles = {
  baseline: { rate: 1, duration: '60s' },
  peak: { rate: 5, duration: '120s' },
  spike: { rate: 20, duration: '30s' },
  soak: { rate: 1, duration: '10m' },
  breaking_point: { rate: 50, duration: '60s' },
};

const scenarioName = __ENV.SCENARIO || 'baseline';
const profile = profiles[scenarioName];
if (!profile) throw new Error(`unknown SCENARIO: ${scenarioName}`);
for (const name of ['ADMIN_TOKEN', 'BASE_URL', 'PEER_ID']) {
  if (!__ENV[name]) throw new Error(`${name} is required`);
}

const thresholds = scenarioName === 'breaking_point' ? {} : {
  http_req_failed: ['rate==0'],
  checks: ['rate==1'],
  dropped_iterations: ['count==0'],
  http_req_duration: ['p(95)<200', 'p(99)<500'],
};

export const options = {
  scenarios: {
    reconnect: {
      executor: 'constant-arrival-rate',
      rate: Number(__ENV.RATE || profile.rate),
      timeUnit: '1s',
      duration: __ENV.DURATION || profile.duration,
      preAllocatedVUs: Number(__ENV.PRE_ALLOCATED_VUS || Math.max(10, profile.rate)),
      maxVUs: Number(__ENV.MAX_VUS || Math.max(50, profile.rate * 2)),
    },
  },
  thresholds,
};

export default function () {
  const res = http.post(
    `${__ENV.BASE_URL}/peers/${encodeURIComponent(__ENV.PEER_ID)}/connections/conn-0:reconnect`,
    null,
    { headers: { Authorization: `Bearer ${__ENV.ADMIN_TOKEN}` } },
  );
  check(res, { 'reconnect accepted': (r) => r.status === 202 });
}
