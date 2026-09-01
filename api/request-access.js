// Vercel serverless function: POST /api/request-access
// Accepts a benchmark-access request, validates it, logs/forwards the
// telemetry, and returns a structured JSON response.

const EMAIL_RE = /^[^\s@]+@[^\s@]+\.[^\s@]+$/;
const REQUIRED_FIELDS = ['name', 'email', 'orgType', 'coreFootprint', 'useCase'];
const MAX_FIELD_LENGTH = 2000;
const WEBHOOK_TIMEOUT_MS = 5000;

function sanitize(value) {
  return typeof value === 'string' ? value.trim().slice(0, MAX_FIELD_LENGTH) : '';
}

module.exports = async function handler(req, res) {
  if (req.method !== 'POST') {
    res.setHeader('Allow', 'POST');
    return res.status(405).json({ success: false, error: 'Method not allowed. Use POST.' });
  }

  let body = req.body;
  if (typeof body === 'string') {
    try {
      body = JSON.parse(body);
    } catch (err) {
      return res.status(400).json({ success: false, error: 'Malformed JSON payload.' });
    }
  }
  if (!body || typeof body !== 'object') {
    return res.status(400).json({ success: false, error: 'Missing request payload.' });
  }

  // Honeypot field: accept silently so bots don't learn the check exists.
  if (body.botcheck) {
    return res.status(200).json({ success: true, message: 'Request received' });
  }

  const fields = {
    name: sanitize(body.name),
    email: sanitize(body.email),
    organization: sanitize(body.organization),
    orgType: sanitize(body.orgType),
    coreFootprint: sanitize(body.coreFootprint),
    useCase: sanitize(body.useCase),
  };

  const missing = REQUIRED_FIELDS.filter((field) => !fields[field]);
  if (missing.length > 0) {
    return res.status(400).json({
      success: false,
      error: `Missing required field(s): ${missing.join(', ')}`,
    });
  }

  if (!EMAIL_RE.test(fields.email)) {
    return res.status(400).json({ success: false, error: 'Invalid email address.' });
  }

  const forwardedFor = req.headers['x-forwarded-for'];
  const record = {
    ...fields,
    receivedAt: new Date().toISOString(),
    ip: (typeof forwardedFor === 'string' ? forwardedFor.split(',')[0].trim() : '') ||
      req.socket?.remoteAddress || '',
  };

  try {
    // Clean structured log — always happens, visible in Vercel's function logs.
    console.log('[request-access] telemetry access request', JSON.stringify(record));

    const webhookUrl = process.env.REQUEST_ACCESS_WEBHOOK_URL;
    if (webhookUrl) {
      const controller = new AbortController();
      const timeout = setTimeout(() => controller.abort(), WEBHOOK_TIMEOUT_MS);
      try {
        const webhookRes = await fetch(webhookUrl, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(record),
          signal: controller.signal,
        });
        if (!webhookRes.ok) {
          console.error('[request-access] webhook forward failed', webhookRes.status);
        }
      } finally {
        clearTimeout(timeout);
      }
    }
  } catch (err) {
    // Forwarding is best-effort: the request was already logged above, so a
    // downstream notification failure should not fail the client's request.
    console.error('[request-access] webhook forward error', err && err.message);
  }

  return res.status(200).json({ success: true, message: 'Request received' });
};
