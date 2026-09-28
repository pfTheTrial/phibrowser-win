'use strict';
const params = new URLSearchParams(location.search);
const port = Number(params.get('port'));
const token = params.get('token');
const caseId = params.get('case');
if (!Number.isInteger(port) || port < 1 || port > 65535 ||
    !/^[a-f0-9]+$/.test(token || '') || !/^[a-z]+$/.test(caseId || '')) {
  throw new Error('Missing isolated popup test configuration');
}
const endpoint = `http://127.0.0.1:${port}/${token}/${caseId}`;
const image = document.createElement('img');
image.src = `${endpoint}/image`;
document.body.appendChild(image);
// A loopback image beacon proves the popup's real window.load fired. No CDP
// attachment, remote executable code, permissions, or extension storage.
let loadedBeacon;
window.addEventListener('load', () => {
  document.getElementById('status').textContent = 'Popup fully loaded';
  document.title = `Hosted popup audit loaded ${caseId}`;
  loadedBeacon = new Image();
  loadedBeacon.src = `${endpoint}/loaded`;
}, { once: true });
