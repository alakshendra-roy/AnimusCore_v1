// Vercel serverless function: POST /api/request-access
// Accepts a benchmark-access request, validates it, logs/forwards the
// telemetry, and returns a structured JSON response.

const EMAIL_RE = /^[^\s@]+@[^\s@]+\.[^\s@]+$/;
const REQUIRED_FIELDS = ['name', 'email', 'orgType', 'coreFootprint', 'useCase'];
const MAX_FIELD_LENGTH = 2000;
const WEBHOOK_TIMEOUT_MS = 5000;
const RESEND_TIMEOUT_MS = 8000;
const RESEND_API_URL = 'https://api.resend.com/emails';
const NOTIFY_TO = process.env.REQUEST_ACCESS_NOTIFY_EMAIL || 'access@animusinfra.com';
const NOTIFY_FROM = process.env.REQUEST_ACCESS_FROM_EMAIL || 'Animus Engine <noreply@animusinfra.com>';

function sanitize(value) {
  return typeof value === 'string' ? value.trim().slice(0, MAX_FIELD_LENGTH) : '';
}

function escapeHtml(value) {
  return String(value)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

function buildEmailText(record) {
  return [
    'New benchmark access request',
    '',
    `Name:            ${record.name}`,
    `Email:           ${record.email}`,
    `Organization:    ${record.organization || '(not provided)'}`,
    `Org Type:        ${record.orgType}`,
    `Core Footprint:  ${record.coreFootprint}`,
    `Use Case:        ${record.useCase}`,
    `HW Fingerprint:  ${record.hardwareFingerprint || '(not provided — send get_fingerprint.ps1 instructions)'}`,
    '',
    `Received:  ${record.receivedAt}`,
    `IP:        ${record.ip || '(unknown)'}`,
  ].join('\n');
}

function buildEmailHtml(record) {
  const row = (label, value) =>
    `<tr><td style="padding:4px 12px 4px 0;color:#64748b;white-space:nowrap;">${escapeHtml(label)}</td>` +
    `<td style="padding:4px 0;color:#0f172a;">${escapeHtml(value)}</td></tr>`;
  return [
    '<div style="font-family:monospace,sans-serif;font-size:14px;">',
    '<h2 style="margin:0 0 12px;">New Benchmark Access Request</h2>',
    '<table cellpadding="0" cellspacing="0">',
    row('Name', record.name),
    row('Email', record.email),
    row('Organization', record.organization || '(not provided)'),
    row('Org Type', record.orgType),
    row('Core Footprint', record.coreFootprint),
    row('Use Case', record.useCase),
    row('HW Fingerprint', record.hardwareFingerprint || '(not provided)'),
    row('Received', record.receivedAt),
    row('IP', record.ip || '(unknown)'),
    '</table>',
    '</div>',
  ].join('');
}

// Best-effort: a failed/unconfigured send is logged, never thrown, so a
// downstream email-provider outage never turns into a 500 for the visitor
// who already successfully submitted the form (see the record's own
// console.log, which is the durable record of the request regardless of
// whether this send succeeds).
async function sendNotificationEmail(record) {
  const apiKey = process.env.RESEND_API_KEY;
  if (!apiKey) {
    console.warn('[request-access] RESEND_API_KEY not set — skipping email delivery');
    return;
  }

  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), RESEND_TIMEOUT_MS);
  try {
    const resendRes = await fetch(RESEND_API_URL, {
      method: 'POST',
      headers: {
        Authorization: `Bearer ${apiKey}`,
        'Content-Type': 'application/json',
      },
      body: JSON.stringify({
        from: NOTIFY_FROM,
        to: NOTIFY_TO,
        reply_to: record.email,
        subject: `Benchmark Access Request — ${record.organization || record.name}`,
        text: buildEmailText(record),
        html: buildEmailHtml(record),
      }),
      signal: controller.signal,
    });
    if (!resendRes.ok) {
      const body = await resendRes.text().catch(() => '');
      console.error('[request-access] Resend send failed', resendRes.status, body);
    }
  } catch (err) {
    console.error('[request-access] Resend send error', err && err.message);
  } finally {
    clearTimeout(timeout);
  }
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
    hardwareFingerprint: sanitize(body.hardwareFingerprint),
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

    // Primary delivery path: notify inquiries@animusinfra.com directly via Resend.
    await sendNotificationEmail(record);

    // Optional secondary forward (Slack/Zapier/etc.), unrelated to email delivery above.
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
