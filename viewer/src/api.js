/* Tiny REST client for the daemon's /v1/* endpoints.
 *
 * Auth: if localStorage holds `memgraph_api_key`, attach as Bearer.
 * Errors: rejects with { status, message } shaped error.
 */

function getKey() {
  try { return localStorage.getItem('memgraph_api_key') || ''; } catch { return ''; }
}

function headers(extra = {}) {
  const h = { ...extra };
  const k = getKey();
  if (k) h['Authorization'] = `Bearer ${k}`;
  return h;
}

async function getJson(url) {
  const r = await fetch(url, { headers: headers() });
  const text = await r.text();
  let body;
  try { body = text ? JSON.parse(text) : {}; } catch { body = { raw: text }; }
  if (!r.ok) {
    const e = new Error(body.error || r.statusText);
    e.status = r.status;
    throw e;
  }
  return body;
}

async function postJson(url, payload) {
  const r = await fetch(url, {
    method: 'POST',
    headers: headers({ 'Content-Type': 'application/json' }),
    body: JSON.stringify(payload),
  });
  const text = await r.text();
  let body;
  try { body = text ? JSON.parse(text) : {}; } catch { body = { raw: text }; }
  if (!r.ok) {
    const e = new Error(body.error || r.statusText);
    e.status = r.status;
    throw e;
  }
  return body;
}

async function del(url) {
  const r = await fetch(url, { method: 'DELETE', headers: headers() });
  if (!r.ok && r.status !== 204) {
    const text = await r.text().catch(() => '');
    const e = new Error(text || r.statusText);
    e.status = r.status;
    throw e;
  }
  return r.status;
}

export const api = {
  health:   ()                          => getJson(`/v1/healthz`),
  view:     ()                          => getJson(`/v1/view`),
  match:    (text)                      => getJson(`/v1/match?text=${encodeURIComponent(text)}`),
  search:   (text, top_k = 10)          => getJson(`/v1/search?text=${encodeURIComponent(text)}&top_k=${top_k}`),
  explore:  (text, depth = 3, beam = 4, keywords = []) => {
    const kws = (keywords && keywords.length)
      ? `&keywords=${encodeURIComponent(keywords.join(','))}`
      : '';
    return getJson(`/v1/explore?text=${encodeURIComponent(text)}&depth=${depth}&beam=${beam}${kws}`);
  },
  classify: (text)                      => getJson(`/v1/classify?text=${encodeURIComponent(text)}`),
  get:      (id)                        => getJson(`/v1/nodes/${id}`),
  insert:   (payload)                   => postJson(`/v1/insert`, payload),
  remove:   (id)                        => del(`/v1/nodes/${id}`),
};

/* The /v1/match endpoint nests result inside an mpack-style envelope.
 * All endpoints share the shape { status, result, error? }. */
export function unwrap(envelope) {
  if (envelope && typeof envelope === 'object' && 'result' in envelope) {
    return envelope.result;
  }
  return envelope;
}
