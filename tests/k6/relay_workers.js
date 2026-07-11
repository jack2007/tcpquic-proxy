/**
 * Admin /relay/workers load scenarios.
 *
 * Required:
 *   -e ADMIN_TOKEN=...
 * Optional:
 *   -e BASE_URL=http://127.0.0.1:8080/api/v1
 *   -e SCENARIO=console_baseline|console_peak|admin_mixed|admin_spike|admin_soak|breaking_point
 *   -e WORKER_IDS=windows-0,windows-1
 *   -e DURATION=10m
 *   -e VUS=10
 *   -e RATE=10
 *   -e MAX_VUS=64
 *
 * Example:
 *   k6 run -e SCENARIO=admin_mixed -e ADMIN_TOKEN=$TOKEN \
 *     -e BASE_URL=http://127.0.0.1:8080/api/v1 \
 *     -e WORKER_IDS=windows-0 -e DURATION=30s tests/k6/relay_workers.js
 */

import http from 'k6/http';
import { check, sleep } from 'k6';
import { Counter, Rate, Trend } from 'k6/metrics';
import { textSummary } from 'https://jslib.k6.io/k6-summary/0.0.4/index.js';

const BASE_URL = (__ENV.BASE_URL || 'http://127.0.0.1:8080/api/v1').replace(/\/$/, '');
const ADMIN_TOKEN = __ENV.ADMIN_TOKEN || '';
const SCENARIO = (__ENV.SCENARIO || 'console_baseline').toLowerCase();
const WORKER_IDS = (__ENV.WORKER_IDS || '')
  .split(',')
  .map((s) => s.trim())
  .filter(Boolean);
const DURATION = __ENV.DURATION || '';
const VUS = Number(__ENV.VUS || '0');
const RATE = Number(__ENV.RATE || '0');
const MAX_VUS = Number(__ENV.MAX_VUS || '64');

if (!ADMIN_TOKEN) {
  throw new Error('ADMIN_TOKEN is required');
}

const snapshotIncomplete = new Counter('relay_workers_snapshot_incomplete');
const unexpectedStatus = new Counter('relay_workers_unexpected_status');
const expectedNotFound = new Counter('relay_workers_expected_404');
const snapshotCompleteRate = new Rate('relay_workers_snapshot_complete');
const listDuration = new Trend('relay_workers_list_duration', true);
const detailDuration = new Trend('relay_workers_detail_duration', true);

function authHeaders() {
  return {
    Authorization: `Bearer ${ADMIN_TOKEN}`,
    Accept: 'application/json',
  };
}

function tagged(extra) {
  return Object.assign(
    {
      endpoint: 'relay_workers',
      scenario: SCENARIO,
    },
    extra || {},
  );
}

function expectedStatuses(...codes) {
  const set = {};
  for (const code of codes) set[code] = true;
  return set;
}

function parseWorkers(body) {
  try {
    return JSON.parse(body);
  } catch (_) {
    return null;
  }
}

function recordSnapshotComplete(payload) {
  const complete = !!(payload && payload.snapshot_complete);
  snapshotCompleteRate.add(complete);
  if (!complete) snapshotIncomplete.add(1);
  return complete;
}

function getList() {
  const res = http.get(`${BASE_URL}/relay/workers`, {
    headers: authHeaders(),
    tags: tagged({ name: 'workers_list' }),
  });
  listDuration.add(res.timings.duration);
  const ok = check(res, {
    'list status 200': (r) => r.status === 200,
  });
  if (!ok) {
    unexpectedStatus.add(1);
    return null;
  }
  const payload = parseWorkers(res.body);
  recordSnapshotComplete(payload);
  return payload;
}

function getDetail(workerId, allowed) {
  const res = http.get(`${BASE_URL}/relay/workers/${encodeURIComponent(workerId)}`, {
    headers: authHeaders(),
    tags: tagged({ name: 'workers_detail', worker_id: workerId }),
  });
  detailDuration.add(res.timings.duration);
  if (allowed[res.status]) {
    if (res.status === 404) expectedNotFound.add(1);
    if (res.status === 200) {
      const payload = parseWorkers(res.body);
      recordSnapshotComplete(payload);
    }
    return res;
  }
  unexpectedStatus.add(1);
  check(res, {
    'detail status expected': () => false,
  });
  return res;
}

function pickWorkerId(payload) {
  if (WORKER_IDS.length > 0) {
    return WORKER_IDS[Math.floor(Math.random() * WORKER_IDS.length)];
  }
  const rows = (payload && payload.workers) || [];
  const concrete = rows.filter((w) => w && w.worker_id && w.worker_id !== 'aggregate');
  if (concrete.length === 0) return null;
  return concrete[Math.floor(Math.random() * concrete.length)].worker_id;
}

function adminMixedIteration() {
  const roll = Math.random();
  if (roll < 0.8) {
    getList();
    return;
  }
  const payload = getList();
  if (roll < 0.95) {
    const id = pickWorkerId(payload) || 'missing-worker-id';
    getDetail(id, expectedStatuses(200, 404, 503));
    return;
  }
  getDetail(`missing-${__VU}-${__ITER}`, expectedStatuses(404, 503));
}

function consoleIteration() {
  getList();
  sleep(3);
}

function breakingPointIteration() {
  getList();
}

function scenarioOptions() {
  const commonThresholds = {
    http_req_failed: ['rate<0.01'],
    checks: ['rate>0.99'],
    relay_workers_unexpected_status: ['count==0'],
  };

  switch (SCENARIO) {
    case 'console_baseline':
      return {
        scenarios: {
          console_baseline: {
            executor: 'constant-vus',
            vus: VUS > 0 ? VUS : 1,
            duration: DURATION || '10m',
            gracefulStop: '30s',
            tags: tagged(),
          },
        },
        thresholds: Object.assign({}, commonThresholds, {
          relay_workers_list_duration: ['p(99)<3000'],
          relay_workers_snapshot_complete: ['rate>0.95'],
        }),
      };
    case 'console_peak':
      return {
        scenarios: {
          console_peak: {
            executor: 'ramping-vus',
            startVUs: 0,
            stages: [
              { duration: '1m', target: VUS > 0 ? VUS : 10 },
              { duration: DURATION || '5m', target: VUS > 0 ? VUS : 10 },
              { duration: '1m', target: 0 },
            ],
            gracefulStop: '30s',
            tags: tagged(),
          },
        },
        thresholds: Object.assign({}, commonThresholds, {
          relay_workers_list_duration: ['p(99)<3000'],
        }),
      };
    case 'admin_mixed':
      return {
        scenarios: {
          admin_mixed: {
            executor: 'constant-arrival-rate',
            rate: RATE > 0 ? RATE : 10,
            timeUnit: '1s',
            duration: DURATION || '5m',
            preAllocatedVUs: Math.max(VUS > 0 ? VUS : 10, 10),
            maxVUs: MAX_VUS,
            gracefulStop: '30s',
            tags: tagged(),
          },
        },
        thresholds: Object.assign({}, commonThresholds, {
          relay_workers_list_duration: ['p(99)<3000'],
          relay_workers_detail_duration: ['p(99)<3000'],
        }),
      };
    case 'admin_spike':
      return {
        scenarios: {
          admin_spike: {
            executor: 'ramping-arrival-rate',
            startRate: 0,
            timeUnit: '1s',
            preAllocatedVUs: Math.max(VUS > 0 ? VUS : 32, 32),
            maxVUs: MAX_VUS,
            stages: [
              { duration: '30s', target: RATE > 0 ? RATE : 32 },
              { duration: '1m', target: RATE > 0 ? RATE : 32 },
              { duration: '30s', target: 0 },
            ],
            gracefulStop: '30s',
            tags: tagged(),
          },
        },
        thresholds: Object.assign({}, commonThresholds, {
          http_req_failed: ['rate<0.05'],
        }),
      };
    case 'admin_soak':
      return {
        scenarios: {
          admin_soak: {
            executor: 'constant-arrival-rate',
            rate: RATE > 0 ? RATE : 5,
            timeUnit: '1s',
            duration: DURATION || '30m',
            preAllocatedVUs: Math.max(VUS > 0 ? VUS : 10, 10),
            maxVUs: MAX_VUS,
            gracefulStop: '30s',
            tags: tagged(),
          },
        },
        thresholds: Object.assign({}, commonThresholds, {
          relay_workers_list_duration: ['p(99)<3000'],
        }),
      };
    case 'breaking_point': {
      const baseRate = RATE > 0 ? RATE : 16;
      const stages = [];
      for (let level = 1; level <= 8; level += 1) {
        stages.push({ duration: '1m', target: baseRate * level });
      }
      stages.push({ duration: '30s', target: 0 });
      return {
        scenarios: {
          breaking_point: {
            executor: 'ramping-arrival-rate',
            startRate: baseRate,
            timeUnit: '1s',
            preAllocatedVUs: Math.max(VUS > 0 ? VUS : 32, 32),
            maxVUs: Math.max(MAX_VUS, 128),
            stages,
            gracefulStop: '30s',
            tags: tagged(),
          },
        },
        // Breaking-point intentionally relaxes completeness / failure thresholds;
        // summary export is the gate, not a hard fail on incomplete snapshots.
        thresholds: {
          checks: ['rate>0.5'],
          relay_workers_list_duration: ['p(99)<10000'],
        },
      };
    }
    default:
      throw new Error(`Unknown SCENARIO=${SCENARIO}`);
  }
}

export const options = scenarioOptions();

export default function () {
  switch (SCENARIO) {
    case 'console_baseline':
    case 'console_peak':
      consoleIteration();
      break;
    case 'admin_mixed':
    case 'admin_spike':
    case 'admin_soak':
      adminMixedIteration();
      break;
    case 'breaking_point':
      breakingPointIteration();
      break;
    default:
      throw new Error(`Unknown SCENARIO=${SCENARIO}`);
  }
}

export function handleSummary(data) {
  const summary = {
    scenario: SCENARIO,
    base_url: BASE_URL,
    metrics: {
      http_req_duration: data.metrics.http_req_duration,
      http_req_failed: data.metrics.http_req_failed,
      checks: data.metrics.checks,
      relay_workers_list_duration: data.metrics.relay_workers_list_duration,
      relay_workers_detail_duration: data.metrics.relay_workers_detail_duration,
      relay_workers_snapshot_complete: data.metrics.relay_workers_snapshot_complete,
      relay_workers_snapshot_incomplete: data.metrics.relay_workers_snapshot_incomplete,
      relay_workers_unexpected_status: data.metrics.relay_workers_unexpected_status,
      relay_workers_expected_404: data.metrics.relay_workers_expected_404,
    },
  };
  return {
    stdout: textSummary(data, { indent: ' ', enableColors: true }),
    'artifacts/relay-workers-summary.json': JSON.stringify(summary, null, 2),
  };
}
